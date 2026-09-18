// Minimal LLVM smoke test.
//
// If this program asserts, the LLVM build you're linking against is
// inconsistent with the compiler settings of the translation unit that
// includes its headers. Nothing about Lucid is involved — this is the
// shortest possible program that constructs a FunctionType and a Function.

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
#include <llvm/Support/raw_ostream.h>

int main() {
    llvm::LLVMContext ctx;
    auto mod = std::make_unique<llvm::Module>("smoke", ctx);

    llvm::errs() << "[smoke] ctx       = " << (void*)&ctx << "\n";
    llvm::errs() << "[smoke] mod->ctx  = " << (void*)&mod->getContext() << "\n";

    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::errs() << "[smoke] i32       = " << (void*)i32 << "\n";

    llvm::FunctionType* fnTy = llvm::FunctionType::get(i32, {}, false);
    llvm::errs() << "[smoke] fnTy      = " << (void*)fnTy
                 << " params=" << fnTy->getNumParams() << "\n";

    llvm::Function* fn = llvm::Function::Create(
        fnTy,
        llvm::GlobalValue::ExternalLinkage,
        "main",
        mod.get());
    llvm::errs() << "[smoke] fn        = " << (void*)fn << "\n";

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> b(entry);
    b.CreateRet(llvm::ConstantInt::get(i32, 0));

    llvm::errs() << "[smoke] ---- module IR ----\n";
    mod->print(llvm::errs(), nullptr);

    llvm::errs() << "[smoke] OK\n";
    return 0;
}