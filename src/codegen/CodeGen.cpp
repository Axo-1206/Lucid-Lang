/// @file CodeGen.cpp
/// @brief Implementation of the main code generation orchestrator.

#include "CodeGen.hpp"
#include "core/memory/StringPool.hpp"
#include "core/trace/Trace.hpp"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

namespace codegen {

// =============================================================================
// Public API
// =============================================================================

std::vector<std::unique_ptr<llvm::Module>> generate(
    const std::vector<ModuleAST*>& modules,
    StringPool& p, DiagnosticEngine& d,
    llvm::LLVMContext& context
) {
    std::vector<std::unique_ptr<llvm::Module>> result;
    result.reserve(modules.size());

    CodeGenContext ctx(p, d, context);
    ctx.modules = modules;

    // ─── Phase 1: Generate all modules ───────────────────────────────────
    // All declarations and function bodies are generated.
    // Globals are created with null initializers.
    for (ModuleAST* module : modules) {
        if (!module) continue;
        
        std::string name = p.lookup(module->filePath);
        ctx.module = new llvm::Module(name, context);
        ctx.currentFile = module->filePath;
        ctx.currentModule = module;
        
        // Store mapping
        ctx.llvmModules[module] = ctx.module;
        
        // Generate the module (declarations + bodies)
        generateModule(module, ctx);
        
        result.push_back(std::unique_ptr<llvm::Module>(ctx.module));
    }

    // ─── Phase 2: Generate global initializer ────────────────────────────
    // NOW all symbols from all modules exist!
    // Use the first module as the host for the initializer.
    if (!result.empty() && !ctx.pendingGlobals.empty()) {
        ctx.module = result[0].get();
        generateGlobalInitializer(ctx);
    }

    return result;
}

std::unique_ptr<llvm::Module> generateModule(ModuleAST* module, CodeGenContext& ctx) {
    if (!module || !ctx.module) {
        return nullptr;
    }

    Trace::info("Generating IR for module: ", 
                ctx.pool.lookup(module->filePath));

    // ─── Phase 1: Lower all declarations ──────────────────────────────────
    lowerModuleDeclarations(module, ctx);

    // ─── Phase 2: Lower all function bodies ──────────────────────────────
    lowerModuleBodies(module, ctx);

    // ─── Phase 3: Verify the module ──────────────────────────────────────
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyModule(*ctx.module, &errorStream)) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, module->loc,
                                "LLVM IR verification failed: ", error);
        return nullptr;
    }

    Trace::info("Generated IR successfully for module");
    return std::unique_ptr<llvm::Module>(ctx.module);
}

// =============================================================================
// Module-Level Emission
// =============================================================================

void lowerModuleDeclarations(ModuleAST* module, CodeGenContext& ctx) {
    if (!module) return;

    for (DeclAST* decl : module->decls) {
        if (!decl) continue;
        lowerDeclaration(decl, ctx);
    }
}

void lowerModuleBodies(ModuleAST* module, CodeGenContext& ctx) {
    if (!module) return;

    for (DeclAST* decl : module->decls) {
        if (!decl) continue;
        if (decl->isa<FuncDeclAST>()) {
            lowerFunctionBody(decl->as<FuncDeclAST>(), ctx);
        }
    }
}

// =============================================================================
// Global Initializer Generation
// =============================================================================

void generateGlobalInitializer(CodeGenContext& ctx) {
    if (ctx.pendingGlobals.empty()) {
        return;
    }

    // ─── Sort by dependency order using ModuleAST's dependencyOrder ──────
    std::sort(ctx.pendingGlobals.begin(), ctx.pendingGlobals.end(),
        [&](const CodeGenContext::GlobalInitInfo& a,
            const CodeGenContext::GlobalInitInfo& b) {
            int orderA = a.module ? a.module->dependencyOrder : -1;
            int orderB = b.module ? b.module->dependencyOrder : -1;
            if (orderA != orderB) return orderA < orderB;
            return a.orderInModule < b.orderInModule;
        });

    // ─── Create __init_globals function ───────────────────────────────────
    llvm::FunctionType* initType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx.llvmCtx), false
    );
    
    llvm::Function* initFunc = llvm::Function::Create(
        initType,
        llvm::Function::InternalLinkage,
        "__init_globals",
        ctx.module
    );
    
    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "entry",
        initFunc
    );
    ctx.builder.SetInsertPoint(entryBlock);
    ctx.setCurrentFunction(initFunc);

    // ─── Generate initialization for each global ──────────────────────────
    for (const auto& info : ctx.pendingGlobals) {
        // ─── Lower the initializer expression ────────────────────────────
        // This may reference symbols from other modules, which are now
        // available because all modules are already generated.
        llvm::Value* initValue = lowerExpression(info.init, ctx);
        if (initValue) {
            ctx.builder.CreateStore(initValue, info.global);
        }
    }

    ctx.builder.CreateRetVoid();
    ctx.setCurrentFunction(nullptr);

    // ─── Register as global constructor ────────────────────────────────────
    registerGlobalConstructor(initFunc, ctx);

    Trace::info("Generated global initializer with ", 
                ctx.pendingGlobals.size(), " pending globals");
}

void registerGlobalConstructor(llvm::Function* func, CodeGenContext& ctx) {
    if (!func) return;
    
    llvm::LLVMContext& C = ctx.llvmCtx;
    llvm::Type* i32 = llvm::Type::getInt32Ty(C);
    llvm::Type* i8Ptr = llvm::PointerType::get(C, 0);
    
    // __init_globals has type void(), but global constructors expect i8*()
    llvm::FunctionType* ctorFuncType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(C),
        {i8Ptr},
        false
    );
    llvm::Constant* ctorFuncPtr = llvm::ConstantExpr::getBitCast(
        func,
        llvm::PointerType::get(ctorFuncType, 0)
    );
    
    // Create the constructor entry: { priority, func, data }
    llvm::StructType* ctorStructType = llvm::StructType::get(
        C,
        {i32, llvm::PointerType::get(C, 0), llvm::PointerType::get(C, 0)}
    );
    
    llvm::Constant* ctorEntry = llvm::ConstantStruct::get(
        ctorStructType,
        llvm::ConstantInt::get(i32, 65535),
        ctorFuncPtr,
        llvm::Constant::getNullValue(llvm::PointerType::get(C, 0))
    );
    
    // Append to the global constructor list
    llvm::GlobalVariable* ctorList = ctx.module->getGlobalVariable("llvm.global_ctors");
    if (ctorList) {
        // Append to existing list
        std::vector<llvm::Constant*> existingCtors;
        if (llvm::ConstantArray* existingArray = 
            llvm::dyn_cast<llvm::ConstantArray>(ctorList->getInitializer())) {
            for (auto& op : existingArray->operands()) {
                existingCtors.push_back(llvm::cast<llvm::Constant>(&op));
            }
        }
        existingCtors.push_back(ctorEntry);
        
        llvm::ArrayType* newArrayType = llvm::ArrayType::get(
            ctorStructType,
            existingCtors.size()
        );
        llvm::Constant* newArray = llvm::ConstantArray::get(
            newArrayType,
            existingCtors
        );
        ctorList->setInitializer(newArray);
    } else {
        // Create new global constructor list
        llvm::ArrayType* arrayType = llvm::ArrayType::get(ctorStructType, 1);
        llvm::Constant* arrayInit = llvm::ConstantArray::get(arrayType, {ctorEntry});
        
        new llvm::GlobalVariable(
            *ctx.module,
            arrayType,
            false,
            llvm::GlobalValue::AppendingLinkage,
            arrayInit,
            "llvm.global_ctors"
        );
    }
}

} // namespace codegen