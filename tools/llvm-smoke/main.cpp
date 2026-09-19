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
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/raw_ostream.h>

static llvm::GlobalVariable* addModuleTable(llvm::Module& module,
                                            llvm::LLVMContext& ctx) {
    auto* tableType = llvm::ArrayType::get(
        llvm::PointerType::get(ctx, 0),
        256
    );
    return new llvm::GlobalVariable(
        module,
        tableType,
        /*isConstant=*/false,
        llvm::GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr,
        "__lucid_module_instances"
    );
}

static void testWindowsPathModuleName(llvm::LLVMContext& ctx) {
    llvm::errs() << "[smoke] TEST A: Windows-style module name\n";
    auto mod = std::make_unique<llvm::Module>(
        "C:\\Users\\TaiAx\\Desktop\\Lucid\\tests\\codegen\\test.luc",
        ctx);
    auto* moduleTable = addModuleTable(*mod, ctx);
    llvm::errs() << "[smoke] TEST A table = " << (void*)moduleTable << "\n";

    llvm::Type* i32 = llvm::Type::getInt32Ty(ctx);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(i32, {}, false);
    llvm::Function* fn = llvm::Function::Create(
        fnTy,
        llvm::GlobalValue::ExternalLinkage,
        "__module_size_C__Users_TaiAx_Desktop_Lucid_tests_codegen_test_luc",
        mod.get());
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> builder(entry);
    builder.CreateRet(llvm::ConstantInt::get(i32, 0));

    llvm::errs() << "[smoke] TEST A about to print\n";
    llvm::errs().flush();
    mod->print(llvm::errs(), nullptr);
    llvm::errs() << "[smoke] TEST A printed OK\n";
    llvm::errs().flush();
}

static void testMixedReturnFunctions(llvm::LLVMContext& ctx) {
    llvm::errs() << "[smoke] TEST B: mixed i32/i64 functions\n";
    auto mod = std::make_unique<llvm::Module>("mixed-functions", ctx);
    auto* moduleTable = addModuleTable(*mod, ctx);
    llvm::errs() << "[smoke] TEST B table = " << (void*)moduleTable << "\n";

    llvm::FunctionType* mainType = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(ctx), {}, false);
    llvm::Function* mainFn = llvm::Function::Create(
        mainType,
        llvm::GlobalValue::ExternalLinkage,
        "main",
        mod.get());
    llvm::BasicBlock* mainEntry = llvm::BasicBlock::Create(ctx, "entry", mainFn);
    llvm::IRBuilder<> mainBuilder(mainEntry);
    mainBuilder.CreateRet(llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(ctx), 1));

    llvm::FunctionType* sizeType = llvm::FunctionType::get(
        llvm::Type::getInt64Ty(ctx), {}, false);
    llvm::Function* sizeFn = llvm::Function::Create(
        sizeType,
        llvm::GlobalValue::ExternalLinkage,
        "__module_size_C__Users_TaiAx_Desktop_Lucid_tests_codegen_test_luc",
        mod.get());
    llvm::BasicBlock* sizeEntry = llvm::BasicBlock::Create(
        ctx, "module_size_entry", sizeFn);
    llvm::IRBuilder<> sizeBuilder(sizeEntry);
    sizeBuilder.CreateRet(llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(ctx), 0));

    llvm::errs() << "[smoke] TEST B about to print\n";
    llvm::errs().flush();
    mod->print(llvm::errs(), nullptr);
    llvm::errs() << "[smoke] TEST B printed OK\n";
    llvm::errs().flush();
}

int main() {
    llvm::LLVMContext ctx;
    llvm::errs() << "[smoke] ctx = " << (void*)&ctx << "\n";
    testWindowsPathModuleName(ctx);
    testMixedReturnFunctions(ctx);
    llvm::errs() << "[smoke] ALL TESTS OK\n";
    llvm::errs().flush();
    return 0;
}