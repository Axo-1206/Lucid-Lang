/// @file LLVMIntrinsicEmitter.cpp
/// @brief Implementation of LLVM intrinsic emissions.

#include "LLVMIntrinsicEmitter.hpp"
#include "../support/LLVMHelpers.hpp"
#include "../support/CodeGenPanic.hpp"
#include "../CodeGenType.hpp"

#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IntrinsicInst.h>

#include <unordered_set>
#include <cmath>

namespace codegen {

// ─── Math Intrinsics ──────────────────────────────────────────────────────

llvm::Value* emitLLVMMathIntrinsic(
    const std::string& name,
    std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── min / max ────────────────────────────────────────────────────────
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

    // ─── pow ──────────────────────────────────────────────────────────────
    if (name == "pow") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#pow' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* a = args[0];
        llvm::Value* b = args[1];

        if (a->getType()->isIntegerTy() && b->getType()->isIntegerTy()) {
            a = ctx.builder.CreateSIToFP(a, llvm::Type::getDoubleTy(ctx.llvmCtx));
            b = ctx.builder.CreateSIToFP(b, llvm::Type::getDoubleTy(ctx.llvmCtx));
        }

        llvm::Type* resultType = a->getType();
        if (!resultType->isFloatingPointTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "intrinsic '#pow' requires floating-point or integer arguments");
            return nullptr;
        }

        std::string powName = (resultType->isDoubleTy()) ? "pow" : "powf";
        llvm::FunctionType* powType = llvm::FunctionType::get(
            resultType,
            {resultType, resultType},
            false
        );
        llvm::Function* powFunc = ctx.getOrInsertFunction(powName, powType);
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

    llvm::Function* intrinsic = ctx.getLLVMIntrinsicDecl(id, argTypes);
    if (!intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                               "could not get LLVM intrinsic for '#", name, "'");
        return nullptr;
    }

    return ctx.builder.CreateCall(intrinsic, args);
}

// ─── Memory Intrinsics ────────────────────────────────────────────────────

llvm::Value* emitLLVMMemoryIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
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

    llvm::Function* intrinsic = ctx.getLLVMIntrinsicDecl(id, argTypes);
    if (!intrinsic) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
                               "could not get LLVM intrinsic for '#", name, "'");
        return nullptr;
    }

    if (name == "clz" || name == "ctz") {
        std::vector<llvm::Value*> callArgs = args;
        callArgs.push_back(llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx.llvmCtx), 0));
        return ctx.builder.CreateCall(intrinsic, callArgs);
    }

    return ctx.builder.CreateCall(intrinsic, args);
}

// ─── Atomic Intrinsics ────────────────────────────────────────────────────

llvm::Value* emitLLVMAtomicIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    llvm::AtomicOrdering ordering = llvm::AtomicOrdering::SequentiallyConsistent;
    size_t numValueArgs = args.size();
    
    if (numValueArgs > 0) {
        if (llvm::ConstantDataArray* str = llvm::dyn_cast<llvm::ConstantDataArray>(args.back())) {
            if (str->isString()) {
                std::string orderStr = str->getAsString().str();
                ordering = CodeGenContext::parseOrdering(orderStr);
                numValueArgs--;
            }
        }
    }

    // ─── atomic_load(ptr, ordering) ──────────────────────────────────────
    if (name == "atomic_load") {
        if (numValueArgs < 1) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#atomic_load' requires at least 1 argument");
            return nullptr;
        }
        llvm::Value* ptr = args[0];
        llvm::Type* elemType = ctx.getPointeeType(ptr);
        llvm::LoadInst* load = ctx.builder.CreateLoad(elemType, ptr, "atomic_load");
        load->setAtomic(ordering);
        load->setAlignment(llvm::Align(1));
        return load;
    }

    // ─── atomic_store(ptr, val, ordering) ──────────────────────────────
    if (name == "atomic_store") {
        if (numValueArgs < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#atomic_store' requires at least 2 arguments");
            return nullptr;
        }
        llvm::Value* ptr = args[0];
        llvm::Value* val = args[1];
        llvm::StoreInst* store = ctx.builder.CreateStore(val, ptr);
        store->setAtomic(ordering);
        store->setAlignment(llvm::Align(1));
        return nullptr;
    }

    // ─── atomic_add / sub / and / or / xor ──────────────────────────────
    if (name == "atomic_add" || name == "atomic_sub" || 
        name == "atomic_and" || name == "atomic_or" || name == "atomic_xor") {
        if (numValueArgs < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#", name, "' requires at least 2 arguments");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Value* val = args[1];
        llvm::AtomicRMWInst::BinOp op;

        if (name == "atomic_add") op = llvm::AtomicRMWInst::Add;
        else if (name == "atomic_sub") op = llvm::AtomicRMWInst::Sub;
        else if (name == "atomic_and") op = llvm::AtomicRMWInst::And;
        else if (name == "atomic_or") op = llvm::AtomicRMWInst::Or;
        else if (name == "atomic_xor") op = llvm::AtomicRMWInst::Xor;
        else return nullptr;

        return ctx.builder.CreateAtomicRMW(op, ptr, val, llvm::MaybeAlign(), ordering);
    }

    // ─── atomic_cas(ptr, expected, desired, ordering) ──────────────────
    if (name == "atomic_cas") {
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
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── SIMD Arithmetic (lane-wise) ──────────────────────────────────────
    if (name == "simd_add" || name == "simd_sub" || name == "simd_mul" || name == "simd_div") {
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

        if (name == "simd_add") return ctx.builder.CreateAdd(a, b);
        if (name == "simd_sub") return ctx.builder.CreateSub(a, b);
        if (name == "simd_mul") return ctx.builder.CreateMul(a, b);
        if (name == "simd_div") return ctx.builder.CreateSDiv(a, b);
    }

    // ─── SIMD FMA ──────────────────────────────────────────────────────────
    if (name == "simd_fma") {
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
    if (name == "simd_min" || name == "simd_max") {
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
            pred = (name == "simd_min") ? llvm::CmpInst::ICMP_SLT : llvm::CmpInst::ICMP_SGT;
        } else {
            pred = (name == "simd_min") ? llvm::CmpInst::FCMP_OLT : llvm::CmpInst::FCMP_OGT;
        }

        llvm::Value* cmp = ctx.builder.CreateICmp(pred, a, b);
        return ctx.builder.CreateSelect(cmp, a, b);
    }

    // ─── SIMD Splat ────────────────────────────────────────────────────────
    if (name == "simd_splat") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_splat' requires a scalar argument");
            return nullptr;
        }

        llvm::Type* vecType = getType(ctx, expr->resolvedType);
        if (!vecType || !vecType->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "'#simd_splat' requires a vector return type");
            return nullptr;
        }

        llvm::Value* scalar = args[0];
        return ctx.builder.CreateVectorSplat(
            llvm::cast<llvm::VectorType>(vecType)->getElementCount(),
            scalar
        );
    }

    // ─── SIMD Extract ──────────────────────────────────────────────────────
    if (name == "simd_extract") {
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

        if (llvm::ConstantInt* cidx = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
            uint64_t index = cidx->getZExtValue();
            return ctx.builder.CreateExtractElement(vec, index);
        } else {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "index to '#simd_extract' must be a compile-time constant");
            return nullptr;
        }
    }

    // ─── SIMD Insert ──────────────────────────────────────────────────────
    if (name == "simd_insert") {
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

        if (llvm::ConstantInt* cidx = llvm::dyn_cast<llvm::ConstantInt>(idx)) {
            uint64_t index = cidx->getZExtValue();
            return ctx.builder.CreateInsertElement(vec, val, index);
        } else {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "index to '#simd_insert' must be a compile-time constant");
            return nullptr;
        }
    }

    // ─── SIMD Load/Store ──────────────────────────────────────────────────
    if (name == "simd_load") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_load' requires a pointer argument");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Type* vecType = getType(ctx, expr->resolvedType);
        if (!vecType || !vecType->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "'#simd_load' requires a vector return type");
            return nullptr;
        }

        return ctx.builder.CreateLoad(vecType, ptr);
    }

    if (name == "simd_store") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#simd_store' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Value* val = args[1];

        if (!val->getType()->isVectorTy()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "second argument to '#simd_store' must be a vector type");
            return nullptr;
        }

        ctx.builder.CreateStore(val, ptr);
        return nullptr;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown SIMD intrinsic '#", name, "'");
    return nullptr;
}

// ─── CPU Hint Intrinsics ──────────────────────────────────────────────────

llvm::Value* emitLLVMCPUHintIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

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
    if (name == "fence") {
        llvm::AtomicOrdering ordering = llvm::AtomicOrdering::SequentiallyConsistent;

        if (!args.empty()) {
            if (llvm::ConstantDataArray* str = llvm::dyn_cast<llvm::ConstantDataArray>(args[0])) {
                if (str->isString()) {
                    std::string orderStr = str->getAsString().str();
                    ordering = CodeGenContext::parseOrdering(orderStr);
                }
            }
        }

        ctx.builder.CreateFence(ordering);
        return nullptr;
    }

    // ─── pause ──────────────────────────────────────────────────────────
    if (name == "pause") {
        ctx.builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
        return nullptr;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown CPU hint intrinsic '#", name, "'");
    return nullptr;
}

} // namespace codegen