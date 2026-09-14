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

    // ─── Phase 1: Every module, every declaration, every body ──────────
    //
    // For each module, generateModule runs:
    //   - lowerModuleDeclarations: prototypes and types for `decls`
    //     *and* `specializations`
    //   - lowerModuleBodies: bodies for functions in `decls` and
    //     `specializations`
    //
    // By the end of this loop, every LLVM struct type, every function
    // prototype, and every function body the program needs is in place.
    // Nothing else will be created after this point except the global
    // initializer.
    //
    // `pendingGlobals` accumulates during this phase: each module-level
    // `let` with a non-constant initializer is queued when its
    // llvm::GlobalVariable is created, but its initializer expression is
    // not lowered yet.
    for (ModuleAST* module : modules) {
        if (!module) continue;

        std::string name = p.lookup(module->filePath);
        ctx.module = new llvm::Module(name, context);
        ctx.currentFile = module->filePath;
        ctx.currentModule = module;

        ctx.llvmModules[module] = ctx.module;

        generateModule(module, ctx);

        result.push_back(std::unique_ptr<llvm::Module>(ctx.module));
    }

    // ─── Phase 2: Global initializer ────────────────────────────────────
    //
    // __init_globals is generated once, in the first module, after every
    // declaration and body from every module is in place. It lowers each
    // pending global's initializer expression and stores the result into
    // the global's slot.
    //
    // The sort by (dependencyOrder, orderInModule) inside
    // generateGlobalInitializer ensures globals initialize in an order
    // that respects cross-module dependencies.
    //
    // Specializations are covered by Phase 1: any global initializer that
    // references a specialization will find its LLVM type and any LLVM
    // functions it calls already lowered.
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

    // ─── 1. Parser-produced declarations ───────────────────────────────
    //
    // Templates (which Sema never resolved past their signature), plus
    // non-generic structs, enums, traits, foreign functions, and globals.
    //
    // A non-generic FuncDeclAST gets its LLVM prototype here.
    // A non-generic VarDeclAST gets its llvm::GlobalVariable here (with a
    // null initializer; the actual value is deferred to __init_globals).
    // A StructDeclAST — generic template or concrete — is *not* lowered
    // here as a template: a template has no fields and no LLVM struct
    // type is produced for it. Only concrete structs produce LLVM types.
    for (DeclAST* decl : module->decls) {
        if (!decl) continue;
        lowerDeclaration(decl, ctx);
    }

    // ─── 2. Specializations Sema built while resolving this module ─────
    //
    // These are the concrete forms of every generic struct and function
    // the program actually referenced. Each is a StructDeclAST or
    // FuncDeclAST with `genericParams.empty()` and `isGeneric() == false`.
    //
    // Ordering: Sema resolved specializations depth-first, appending each
    // to `module->specializations` only after its body was fully resolved.
    // A specialization's dependencies appear before it in this list, so
    // lowering front-to-back creates every LLVM object before anything
    // that references it.
    for (DeclAST* spec : module->specializations) {
        if (!spec) continue;
        lowerDeclaration(spec, ctx);
    }
}

void lowerModuleBodies(ModuleAST* module, CodeGenContext& ctx) {
    if (!module) return;

    // ─── 1. Bodies of parser-produced functions ────────────────────────
    for (DeclAST* decl : module->decls) {
        if (!decl) continue;
        if (decl->isa<FuncDeclAST>()) {
            lowerFunctionBody(decl->as<FuncDeclAST>(), ctx);
        }
    }

    // ─── 2. Bodies of specialized functions ────────────────────────────
    //
    // Same ordering guarantee as the declaration pass. A specialized
    // function's body may call other specialized functions; those
    // callees appear earlier in the list and their LLVM prototypes
    // already exist from the declaration pass.
    for (DeclAST* spec : module->specializations) {
        if (!spec) continue;
        if (spec->isa<FuncDeclAST>()) {
            lowerFunctionBody(spec->as<FuncDeclAST>(), ctx);
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

    // ─── Sort by module dependency order ──────────────────────────────────
    // ModuleAST::dependencyOrder is set by ModuleResolver.
    // This ensures globals are initialized in the correct order across modules.
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
        {i8Ptr},  // takes a void* data pointer
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
        llvm::ConstantInt::get(i32, 65535),  // priority (default)
        ctorFuncPtr,                          // function
        llvm::Constant::getNullValue(llvm::PointerType::get(C, 0))  // data
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