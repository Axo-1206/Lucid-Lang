/// @file LLVMIntrinsicEmitter.cpp
/// @brief Implementation of LLVM intrinsic emissions.

#include "LLVMIntrinsicEmitter.hpp"
#include "../types/LLVMTypeHelpers.hpp"
#include "../support/CodeGenPanic.hpp"
#include "../types/CodeGenType.hpp"

#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IntrinsicInst.h>

#include <cmath>

namespace codegen {

// ─── Helper: Recover a compile-time string literal from an AST argument ──
//
// By the time an argument reaches `args` it has already been lowered to
// LLVM IR - a Lucid string literal becomes a runtime {ptr,len,cap}
// never a raw llvm::ConstantDataArray. So the *compile-time* text (e.g. a
// memory ordering name like "acquire") can only be recovered from the AST
// node itself, not from the lowered llvm::Value.
static bool tryGetStringLiteralArg(
    IntrinsicCallExprAST* expr,
    size_t index,
    CodeGenContext& ctx,
    std::string& outStr
) {
    if (!expr || index >= expr->args.size()) return false;
    ExprAST* argExpr = expr->args[index];
    if (auto* lit = argExpr->as<LiteralExprAST>()) {
        if (lit->kind == LiteralKind::String || lit->kind == LiteralKind::RawString) {
            outStr = ctx.pool.lookup(lit->value);
            return true;
        }
    }
    return false;
}

// ─── Math Intrinsics ──────────────────────────────────────────────────────

llvm::Value* emitLLVMMathIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── min / max ────────────────────────────────────────────────────────
    if (kind == IntrinsicKind::Min || kind == IntrinsicKind::Max) {
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
            llvm::CmpInst::Predicate pred = (kind == IntrinsicKind::Min)
                ? llvm::CmpInst::ICMP_SLT
                : llvm::CmpInst::ICMP_SGT;
            llvm::Value* cmp = ctx.builder.CreateICmp(pred, a, b);
            return ctx.builder.CreateSelect(cmp, a, b);
        } else if (a->getType()->isFloatingPointTy()) {
            llvm::CmpInst::Predicate pred = (kind == IntrinsicKind::Min)
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

    // ─── abs ──────────────────────────────────────────────────────────────
    // Integer and floating-point abs are different LLVM intrinsics with
    // different signatures - dispatch on the operand type rather than
    // always emitting fabs (which asserts on integer operands).
    if (kind == IntrinsicKind::Abs) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#abs' requires an argument");
            return nullptr;
        }

        llvm::Value* val = args[0];

        if (val->getType()->isIntegerTy()) {
            llvm::Function* absFn = ctx.getLLVMIntrinsicDecl(
                llvm::Intrinsic::abs, {val->getType()}
            );
            if (!absFn) {
                ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                                       "could not get LLVM abs intrinsic");
                return nullptr;
            }
            // is_int_min_poison = false: saturate rather than poison at INT_MIN.
            llvm::Value* isIntMinPoison =
                llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.llvmCtx), 0);
            return ctx.builder.CreateCall(absFn, {val, isIntMinPoison});
        }

        if (val->getType()->isFloatingPointTy()) {
            llvm::Function* fabsFn = ctx.getLLVMIntrinsicDecl(
                llvm::Intrinsic::fabs, {val->getType()}
            );
            if (!fabsFn) {
                ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                                       "could not get LLVM fabs intrinsic");
                return nullptr;
            }
            return ctx.builder.CreateCall(fabsFn, {val});
        }

        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                               "intrinsic '#abs' requires a numeric argument");
        return nullptr;
    }

    // ─── sqrt / fma / ceil / floor / round / trunc / pow ──────────────────────────
    // pow needs its integer-argument promotion handled before it can join
    // the shared llvm::Intrinsic dispatch below (llvm.pow has no integer
    // overload) - everything else here already arrives as the right type.
    if (kind == IntrinsicKind::Pow) {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#pow' requires 2 arguments");
            return nullptr;
        }
        if (args[0]->getType()->isIntegerTy() && args[1]->getType()->isIntegerTy()) {
            args[0] = ctx.builder.CreateSIToFP(args[0], llvm::Type::getDoubleTy(ctx.llvmCtx));
            args[1] = ctx.builder.CreateSIToFP(args[1], llvm::Type::getDoubleTy(ctx.llvmCtx));
        }
        if (!args[0]->getType()->isFloatingPointTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "intrinsic '#pow' requires floating-point or integer arguments");
            return nullptr;
        }
    }

    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    switch (kind) {
        case IntrinsicKind::Sqrt:  id = llvm::Intrinsic::sqrt;  break;
        case IntrinsicKind::Fma:   id = llvm::Intrinsic::fma;   break;
        case IntrinsicKind::Ceil:  id = llvm::Intrinsic::ceil;  break;
        case IntrinsicKind::Floor: id = llvm::Intrinsic::floor; break;
        case IntrinsicKind::Round: id = llvm::Intrinsic::round; break;
        case IntrinsicKind::Trunc: id = llvm::Intrinsic::trunc; break;
        case IntrinsicKind::Pow:   id = llvm::Intrinsic::pow;   break;
        default: break;
    }

    if (id == llvm::Intrinsic::not_intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                               "unknown math intrinsic '#", name, "'");
        return nullptr;
    }

    size_t requiredArgs = 1;
    if (kind == IntrinsicKind::Fma) requiredArgs = 3;
    else if (kind == IntrinsicKind::Pow) requiredArgs = 2;

    if (args.size() < requiredArgs) {
        ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                               "intrinsic '#", name, "' requires ",
                               requiredArgs, requiredArgs == 1 ? " argument" : " arguments");
        return nullptr;
    }

    // These are all overloaded on a SINGLE type slot (every operand shares
    // the same type via LLVMMatchType<0>), so only the first operand's type
    // is passed - passing one type per argument (e.g. 3 for fma) does not
    // match the intrinsic's overload signature.
    llvm::Function* intrinsic = ctx.getLLVMIntrinsicDecl(id, {args[0]->getType()});
    if (!intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                               "could not get LLVM intrinsic for '#", name, "'");
        return nullptr;
    }

    return ctx.builder.CreateCall(intrinsic, args);
}

// ─── Memory Intrinsics ────────────────────────────────────────────────────

llvm::Value* emitLLVMMemoryIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    switch (kind) {
        case IntrinsicKind::Memcpy:  id = llvm::Intrinsic::memcpy;  break;
        case IntrinsicKind::Memmove: id = llvm::Intrinsic::memmove; break;
        case IntrinsicKind::Memset:  id = llvm::Intrinsic::memset;  break;
        default: break;
    }

    if (id == llvm::Intrinsic::not_intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                               "unknown memory intrinsic '#", name, "'");
        return nullptr;
    }

    llvm::SmallVector<llvm::Type*, 4> argTypes;
    for (llvm::Value* arg : args) {
        argTypes.push_back(arg->getType());
    }

    llvm::Function* intrinsic = ctx.getLLVMIntrinsicDecl(id, argTypes);
    if (!intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                               "could not get LLVM intrinsic for '#", name, "'");
        return nullptr;
    }

    std::vector<llvm::Value*> callArgs = args;
    if (callArgs.size() < 4) {
        callArgs.push_back(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.llvmCtx), 0));
    }

    return ctx.builder.CreateCall(intrinsic, callArgs);
}

// ─── Bit Intrinsics ───────────────────────────────────────────────────────

llvm::Value* emitLLVMBitIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    if (args.empty()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                               "intrinsic '#", name, "' requires an argument");
        return nullptr;
    }

    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    switch (kind) {
        case IntrinsicKind::Clz:      id = llvm::Intrinsic::ctlz;  break;
        case IntrinsicKind::Ctz:      id = llvm::Intrinsic::cttz;  break;
        case IntrinsicKind::Popcount: id = llvm::Intrinsic::ctpop; break;
        case IntrinsicKind::Bswap:    id = llvm::Intrinsic::bswap; break;
        default: break;
    }

    if (id == llvm::Intrinsic::not_intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                               "unknown bit intrinsic '#", name, "'");
        return nullptr;
    }

    llvm::SmallVector<llvm::Type*, 4> argTypes;
    argTypes.push_back(args[0]->getType());

    llvm::Function* intrinsic = ctx.getLLVMIntrinsicDecl(id, argTypes);
    if (!intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                               "could not get LLVM intrinsic for '#", name, "'");
        return nullptr;
    }

    if (kind == IntrinsicKind::Clz || kind == IntrinsicKind::Ctz) {
        std::vector<llvm::Value*> callArgs = args;
        callArgs.push_back(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.llvmCtx), 0));
        return ctx.builder.CreateCall(intrinsic, callArgs);
    }

    return ctx.builder.CreateCall(intrinsic, args);
}

// ─── Atomic Intrinsics ────────────────────────────────────────────────────

llvm::Value* emitLLVMAtomicIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    llvm::AtomicOrdering ordering = llvm::AtomicOrdering::SequentiallyConsistent;
    size_t numValueArgs = args.size();

    if (numValueArgs > 0) {
        std::string orderStr;
        if (tryGetStringLiteralArg(expr, numValueArgs - 1, ctx, orderStr)) {
            ordering = parseAtomicOrdering(orderStr);
            numValueArgs--;
        }
    }

    // ─── atomic_load(ptr, ordering) ──────────────────────────────────────
    if (kind == IntrinsicKind::AtomicLoad) {
        if (numValueArgs < 1) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#atomic_load' requires at least 1 argument");
            return nullptr;
        }
        llvm::Value* ptr = args[0];

        // ctx.getPointeeType() is only a stub that always returns i8 (opaque
        // pointers carry no element type). The real loaded type is the
        // intrinsic call's own resolved type (atomic_load(ptr) -> T).
        llvm::Type* elemType = expr ? getType(ctx, expr->resolvedType) : nullptr;
        if (!elemType) {
            elemType = ctx.getPointeeType(ptr);
        }

        llvm::LoadInst* load = ctx.builder.CreateLoad(elemType, ptr, "atomic_load");
        load->setAtomic(ordering);
        load->setAlignment(llvm::Align(getTypeAlign(elemType, ctx.module)));
        return load;
    }

    // ─── atomic_store(ptr, val, ordering) ──────────────────────────────
    if (kind == IntrinsicKind::AtomicStore) {
        if (numValueArgs < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#atomic_store' requires at least 2 arguments");
            return nullptr;
        }
        llvm::Value* ptr = args[0];
        llvm::Value* val = args[1];
        llvm::StoreInst* store = ctx.builder.CreateStore(val, ptr);
        store->setAtomic(ordering);
        store->setAlignment(llvm::Align(getTypeAlign(val->getType(), ctx.module)));
        return nullptr;
    }

    // ─── atomic_add / sub / and / or / xor ──────────────────────────────
    if (kind == IntrinsicKind::AtomicAdd || kind == IntrinsicKind::AtomicSub ||
        kind == IntrinsicKind::AtomicAnd || kind == IntrinsicKind::AtomicOr ||
        kind == IntrinsicKind::AtomicXor) {
        if (numValueArgs < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#", name, "' requires at least 2 arguments");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Value* val = args[1];
        llvm::AtomicRMWInst::BinOp op;

        switch (kind) {
            case IntrinsicKind::AtomicAdd: op = llvm::AtomicRMWInst::Add; break;
            case IntrinsicKind::AtomicSub: op = llvm::AtomicRMWInst::Sub; break;
            case IntrinsicKind::AtomicAnd: op = llvm::AtomicRMWInst::And; break;
            case IntrinsicKind::AtomicOr:  op = llvm::AtomicRMWInst::Or;  break;
            case IntrinsicKind::AtomicXor: op = llvm::AtomicRMWInst::Xor; break;
            default: return nullptr;
        }

        return ctx.builder.CreateAtomicRMW(op, ptr, val, llvm::MaybeAlign(), ordering);
    }

    // ─── atomic_cas(ptr, expected, desired, ordering) ──────────────────
    if (kind == IntrinsicKind::AtomicCas) {
        if (numValueArgs < 3) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#atomic_cas' requires at least 3 arguments");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Value* expected = args[1];
        llvm::Value* desired = args[2];

        llvm::AtomicCmpXchgInst* cas = ctx.builder.CreateAtomicCmpXchg(
            ptr, expected, desired, llvm::MaybeAlign(), ordering,
            llvm::AtomicOrdering::SequentiallyConsistent
        );
        return ctx.builder.CreateExtractValue(cas, 1);
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown atomic intrinsic '#", name, "'");
    return nullptr;
}

// ─── SIMD Intrinsics ──────────────────────────────────────────────────────

llvm::Value* emitLLVMSIMDIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── Helper: Get the vector type from the expression's resolved type ──
    auto getVectorType = [&]() -> llvm::VectorType* {
        if (!expr || !expr->resolvedType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "intrinsic '#", name, "' has no resolved type");
            return nullptr;
        }
        llvm::Type* llvmType = getType(ctx, expr->resolvedType);
        if (!llvmType || !llvmType->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "intrinsic '#", name, "' requires a vector return type");
            return nullptr;
        }
        return llvm::cast<llvm::VectorType>(llvmType);
    };

    // ─── Helper: Get constant integer value ──────────────────────────────
    auto getConstantIntValue = [](llvm::Value* val) -> uint64_t {
        if (auto* cint = llvm::dyn_cast<llvm::ConstantInt>(val)) {
            return cint->getZExtValue();
        }
        return 0;
    };

    // ─── Helper: Perform bounds check and emit panic if out of bounds ────
    auto checkIndexBounds = [&](uint64_t index, uint64_t laneCount, 
                                const std::string& operation) -> bool {
        if (index >= laneCount) {
            emitPanic(RuntimeErrorKind::ArrayIndexOutOfBounds, ctx,
                      "SIMD " + operation + " index " + std::to_string(index) + 
                      " out of bounds for vector of length " + std::to_string(laneCount),
                      loc);
            return false;
        }
        return true;
    };

    // ─── SIMD Arithmetic (lane-wise) ──────────────────────────────────────
    // #simd_add, #simd_sub, #simd_mul, #simd_div
    if (kind == IntrinsicKind::SimdAdd || kind == IntrinsicKind::SimdSub ||
        kind == IntrinsicKind::SimdMul || kind == IntrinsicKind::SimdDiv) {
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

        if (!a->getType()->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "arguments to '#", name, "' must be vector types");
            return nullptr;
        }

        bool isFloat = a->getType()->getScalarType()->isFloatingPointTy();

        switch (kind) {
            case IntrinsicKind::SimdAdd:
                return isFloat ? ctx.builder.CreateFAdd(a, b) : ctx.builder.CreateAdd(a, b);
            case IntrinsicKind::SimdSub:
                return isFloat ? ctx.builder.CreateFSub(a, b) : ctx.builder.CreateSub(a, b);
            case IntrinsicKind::SimdMul:
                return isFloat ? ctx.builder.CreateFMul(a, b) : ctx.builder.CreateMul(a, b);
            case IntrinsicKind::SimdDiv:
                return isFloat ? ctx.builder.CreateFDiv(a, b) : ctx.builder.CreateSDiv(a, b);
            default:
                return nullptr;
        }
    }

    // ─── SIMD FMA ──────────────────────────────────────────────────────────
    // #simd_fma(a, b, c)
    if (kind == IntrinsicKind::SimdFma) {
        if (args.size() < 3) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_fma' requires 3 arguments");
            return nullptr;
        }

        llvm::Value* a = args[0];
        llvm::Value* b = args[1];
        llvm::Value* c = args[2];

        if (a->getType() != b->getType() || a->getType() != c->getType()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "all arguments to '#simd_fma' must have the same type");
            return nullptr;
        }

        if (!a->getType()->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "arguments to '#simd_fma' must be vector types");
            return nullptr;
        }

        llvm::SmallVector<llvm::Type*, 4> argTypes = {a->getType(), b->getType(), c->getType()};
        llvm::Function* fma = ctx.getLLVMIntrinsicDecl(llvm::Intrinsic::fma, argTypes);
        if (!fma) {
            ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                                   "could not get LLVM fma intrinsic");
            return nullptr;
        }

        return ctx.builder.CreateCall(fma, {a, b, c});
    }

    // ─── SIMD Min/Max ──────────────────────────────────────────────────────
    // #simd_min, #simd_max
    if (kind == IntrinsicKind::SimdMin || kind == IntrinsicKind::SimdMax) {
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

        if (!a->getType()->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "arguments to '#", name, "' must be vector types");
            return nullptr;
        }

        llvm::CmpInst::Predicate pred;
        if (a->getType()->getScalarType()->isIntegerTy()) {
            pred = (kind == IntrinsicKind::SimdMin) ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_SGT;
        } else {
            pred = (kind == IntrinsicKind::SimdMin) ? llvm::CmpInst::FCMP_OLT : llvm::CmpInst::FCMP_OGT;
        }

        llvm::Value* cmp = ctx.builder.CreateICmp(pred, a, b);
        return ctx.builder.CreateSelect(cmp, a, b);
    }

    // ─── SIMD Splat ────────────────────────────────────────────────────────
    // #simd_splat(scalar)
    // Note: type_enum and lanes_enum are compile-time constants consumed by Sema.
    // The resolved type is Simd<T,N> which maps to the vector type.
    if (kind == IntrinsicKind::SimdSplat) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_splat' requires a scalar argument");
            return nullptr;
        }

        llvm::VectorType* vecType = getVectorType();
        if (!vecType) return nullptr;

        llvm::Value* scalar = args[0];
        
        // Validate scalar type matches vector element type
        llvm::Type* scalarType = scalar->getType();
        llvm::Type* elemType = vecType->getElementType();
        if (scalarType != elemType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "scalar type does not match vector element type");
            return nullptr;
        }

        return ctx.builder.CreateVectorSplat(
            vecType->getElementCount(),
            scalar
        );
    }

    // ─── SIMD Load ────────────────────────────────────────────────────────
    // #simd_load(ptr)
    // Note: lanes_enum is a compile-time constant consumed by Sema.
    // The resolved type is Simd<T,N> which maps to the vector type.
    if (kind == IntrinsicKind::SimdLoad) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_load' requires a pointer argument");
            return nullptr;
        }

        llvm::VectorType* vecType = getVectorType();
        if (!vecType) return nullptr;

        llvm::Value* ptr = args[0];

        // Validate pointer type - should be a pointer
        if (!ptr->getType()->isPointerTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "argument to '#simd_load' must be a pointer");
            return nullptr;
        }

        // With opaque pointers, we can't check the pointee type directly.
        // Sema already validated this, so we trust it.

        return ctx.builder.CreateLoad(vecType, ptr);
    }

    // ─── SIMD Store ────────────────────────────────────────────────────────
    // #simd_store(ptr, simd_value)
    if (kind == IntrinsicKind::SimdStore) {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_store' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Value* val = args[1];

        if (!ptr->getType()->isPointerTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "first argument to '#simd_store' must be a pointer");
            return nullptr;
        }

        if (!val->getType()->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "second argument to '#simd_store' must be a vector type");
            return nullptr;
        }

        // Sema validated element type match, so we trust it.

        ctx.builder.CreateStore(val, ptr);
        return nullptr;
    }

    // ─── SIMD Extract ──────────────────────────────────────────────────────
    // #simd_extract(v, index)
    if (kind == IntrinsicKind::SimdExtract) {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_extract' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* vec = args[0];
        llvm::Value* idx = args[1];

        if (!vec->getType()->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "first argument to '#simd_extract' must be a vector type");
            return nullptr;
        }

        llvm::VectorType* vecType = llvm::cast<llvm::VectorType>(vec->getType());
        uint64_t laneCount = vecType->getElementCount().getKnownMinValue();

        // Index must be a compile-time constant (Sema already validates this)
        if (!isConstantInt(idx)) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "index to '#simd_extract' must be a compile-time constant");
            return nullptr;
        }

        uint64_t index = getConstantIntValue(idx);
        
        // ─── Bounds check ──────────────────────────────────────────────────
        if (!checkIndexBounds(index, laneCount, "extract")) {
            return nullptr;
        }
        
        return ctx.builder.CreateExtractElement(vec, index);
    }

    // ─── SIMD Insert ──────────────────────────────────────────────────────
    // #simd_insert(v, index, value)
    if (kind == IntrinsicKind::SimdInsert) {
        if (args.size() < 3) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_insert' requires 3 arguments");
            return nullptr;
        }

        llvm::Value* vec = args[0];
        llvm::Value* idx = args[1];
        llvm::Value* val = args[2];

        if (!vec->getType()->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "first argument to '#simd_insert' must be a vector type");
            return nullptr;
        }

        // Validate value type matches vector element type
        llvm::VectorType* vecType = llvm::cast<llvm::VectorType>(vec->getType());
        llvm::Type* elemType = vecType->getElementType();
        if (val->getType() != elemType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "value type does not match vector element type");
            return nullptr;
        }

        uint64_t laneCount = vecType->getElementCount().getKnownMinValue();

        // Index must be a compile-time constant (Sema already validates this)
        if (!isConstantInt(idx)) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "index to '#simd_insert' must be a compile-time constant");
            return nullptr;
        }

        uint64_t index = getConstantIntValue(idx);
        
        // ─── Bounds check ──────────────────────────────────────────────────
        if (!checkIndexBounds(index, laneCount, "insert")) {
            return nullptr;
        }
        
        return ctx.builder.CreateInsertElement(vec, val, index);
    }

    // ─── Unknown SIMD intrinsic ──────────────────────────────────────────
    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown SIMD intrinsic '#", name, "'");
    return nullptr;
}

// ─── CPU Hint Intrinsics ──────────────────────────────────────────────────

llvm::Value* emitLLVMCPUHintIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── prefetch ────────────────────────────────────────────────────────
    if (kind == IntrinsicKind::Prefetch || kind == IntrinsicKind::PrefetchR ||
        kind == IntrinsicKind::PrefetchW) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#", name, "' requires an argument");
            return nullptr;
        }

        llvm::Value* ptr = args[0];

        int rw = (kind == IntrinsicKind::PrefetchW) ? 1 : 0;
        int locality = 3;
        int cacheType = 0;

        llvm::Function* prefetch = ctx.getLLVMIntrinsicDecl(
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
    if (kind == IntrinsicKind::Fence) {
        llvm::AtomicOrdering ordering = llvm::AtomicOrdering::SequentiallyConsistent;

        if (!args.empty()) {
            std::string orderStr;
            if (tryGetStringLiteralArg(expr, 0, ctx, orderStr)) {
                ordering = parseAtomicOrdering(orderStr);
            }
        }

        ctx.builder.CreateFence(ordering);
        return nullptr;
    }

    // ─── pause ──────────────────────────────────────────────────────────
    if (kind == IntrinsicKind::Pause) {
        ctx.builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
        return nullptr;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown CPU hint intrinsic '#", name, "'");
    return nullptr;
}

} // namespace codegen