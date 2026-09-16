/// @file CodeGen.cpp
/// @brief Implementation of the main code generation orchestrator.

#include "CodeGen.hpp"
#include "core/memory/StringPool.hpp"
#include "core/trace/Trace.hpp"
#include "memory/CodeGenOwnership.hpp"

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

    // ─── Module ID assignment ────────────────────────────────────────────
    // The ID is the module's index in the topologically-sorted list.
    // This is the contract the interpreter relies on when it fills
    // @__lucid_module_instances: entry i holds the instance pointer for
    // the module whose ID is i. See the header comment on generate().
    for (size_t i = 0; i < modules.size(); ++i) {
        if (modules[i]) ctx.moduleIds[modules[i]] = static_cast<uint32_t>(i);
    }

    // ─── Phase 1: Every module, every declaration, every body ────────────
    for (size_t i = 0; i < modules.size(); ++i) {
        ModuleAST* module = modules[i];
        if (!module) continue;

        std::string name = p.lookup(module->filePath);
        ctx.module = new llvm::Module(name, context);
        ctx.currentFile = module->filePath;
        ctx.currentModule = module;
        ctx.llvmModules[module] = ctx.module;

        // ─── Emit the instance table and size array in the first module ──
        // Must run before any declaration is lowered, so that a module
        // whose access sites reference @__lucid_module_instances finds
        // the global already declared.
        if (i == 0) {
            emitModuleInstanceTable(modules, ctx);
        }

        generateModule(module, ctx);
        result.push_back(std::unique_ptr<llvm::Module>(ctx.module));
    }

    // ─── Phase 2: Per-module __init_module_<name> / __free_module_<name> ─
    //
    // Emitted after the per-module loop, because they lower initializer
    // expressions that may reference symbols from other modules (already
    // lowered in Phase 1).
    //
    // Each generateModuleInit / generateModuleFree call sets
    // ctx.module and ctx.currentModule to the module it's emitting for,
    // then restores them. The functions themselves are placed in the
    // module they belong to — unlike __init_globals, which was emitted
    // once into the first module.
    for (ModuleAST* module : modules) {
        if (!module) continue;

        // Find the llvm::Module we created for this ModuleAST.
        llvm::Module* savedModule = ctx.module;
        ModuleAST* savedCurrentModule = ctx.currentModule;

        ctx.module = ctx.llvmModules[module];
        ctx.currentModule = module;

        generateModuleInit(module, ctx);
        generateModuleFree(module, ctx);

        ctx.module = savedModule;
        ctx.currentModule = savedCurrentModule;
    }

    // ─── Phase 3: Global initializer (unchanged; dead in Commit 3+) ──────
    // Still runs until Commit 5 deletes it. The old lowerGlobalVar path
    // still creates GlobalVariables and queues pendingGlobals; this call
    // still lowers them. After Commit 3, nothing reads the resulting
    // globals, but the code is harmless until Commit 5 removes it.
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

void emitModuleInstanceTable(const std::vector<ModuleAST*>& modules, CodeGenContext& ctx) {
    if (!ctx.module || modules.empty()) return;

    llvm::LLVMContext& C = ctx.llvmCtx;
    const size_t N = modules.size();

    // ─── @__lucid_module_instances : [N x ptr] ──────────────────────────
    // One entry per module, in module-ID order. Zero-initialized; the
    // interpreter fills each slot before any user code runs.
    llvm::ArrayType* tableType = llvm::ArrayType::get(getPtrType(C), N);
    new llvm::GlobalVariable(
        *ctx.module,
        tableType,
        /*isConstant=*/false,
        llvm::GlobalValue::ExternalLinkage,   // interpreter needs to write it
        llvm::Constant::getNullValue(tableType),
        "__lucid_module_instances"
    );

    // ─── @__module_sizes : [N x i64] ────────────────────────────────────
    // The interpreter reads entry i to know how many bytes to malloc for
    // module i's instance. Filled in by CodeGen now (it's a compile-time
    // constant), not by the interpreter.
    llvm::ArrayType* sizeType = llvm::ArrayType::get(getI64Type(C), N);
    std::vector<llvm::Constant*> sizes;
    sizes.reserve(N);
    for (ModuleAST* m : modules) {
        uint64_t sz = 0;
        if (m) {
            // Ensure the layout is computed, then read the type's size.
            llvm::StructType* instTy = getModuleInstanceType(ctx, m);
            if (instTy && instTy->isSized()) {
                sz = ctx.module->getDataLayout()
                          .getTypeAllocSize(instTy).getFixedValue();
            }
        }
        sizes.push_back(llvm::ConstantInt::get(getI64Type(C), sz));
    }
    llvm::Constant* sizeInit = llvm::ConstantArray::get(sizeType, sizes);
    new llvm::GlobalVariable(
        *ctx.module,
        sizeType,
        /*isConstant=*/true,
        llvm::GlobalValue::ExternalLinkage,
        sizeInit,
        "__module_sizes"
    );
}

void generateModuleInit(ModuleAST* module, CodeGenContext& ctx) {
    if (!module || !ctx.module) return;

    ModuleInstanceLayout& layout = ctx.getOrCreateModuleLayout(module);
    if (layout.fields.empty()) return;  // nothing to initialize

    // ─── Create __init_module_<name>(ptr %inst) ─────────────────────────
    std::string funcName =
        "__init_module_" + sanitizeForLLVMSymbol(ctx.pool.lookup(module->filePath));

    llvm::FunctionType* fnType = llvm::FunctionType::get(
        getVoidType(ctx.llvmCtx),
        {getPtrType(ctx.llvmCtx)},   // ptr %inst
        false);
    llvm::Function* fn = llvm::Function::Create(
        fnType,
        llvm::GlobalValue::InternalLinkage,
        funcName,
        ctx.module);
    fn->getArg(0)->setName("inst");

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx.llvmCtx, "entry", fn);
    ctx.builder.SetInsertPoint(entry);
    ctx.setCurrentFunction(fn);

    // ─── For each field, lower the init and store into the instance ─────
    for (size_t i = 0; i < layout.fields.size(); ++i) {
        ValueDeclAST* decl = layout.fields[i];

        if (!decl->isa<VarDeclAST>()) {
            // Reserved slot for a cls-shaped module-level FuncDeclAST.
            // Skipped until the follow-up lands.
            continue;
        }
        VarDeclAST* var = decl->as<VarDeclAST>();
        if (!var->init) continue;   // no initializer means zero-init is enough

        llvm::Value* initValue = lowerExpression(var->init, ctx);
        if (!initValue) continue;

        // Match generateGlobalInitializer's coercion handling.
        initValue = maybeCoerceFnToCls(
            initValue,
            var->init->resolvedType,
            var->type,
            ctx);
        if (!initValue) continue;

        // GEP to the field, store.
        llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
            layout.type,
            fn->getArg(0),
            static_cast<unsigned>(i),
            "init_field_" + ctx.pool.lookup(var->name));
        ctx.builder.CreateStore(initValue, fieldPtr);
    }

    ctx.builder.CreateRetVoid();
    ctx.setCurrentFunction(nullptr);
}

void generateModuleFree(ModuleAST* module, CodeGenContext& ctx) {
    if (!module || !ctx.module) return;

    ModuleInstanceLayout& layout = ctx.getOrCreateModuleLayout(module);
    if (layout.fields.empty()) return;

    // ─── Create __free_module_<name>(ptr %inst) ─────────────────────────
    std::string funcName =
        "__free_module_" + sanitizeForLLVMSymbol(ctx.pool.lookup(module->filePath));

    llvm::FunctionType* fnType = llvm::FunctionType::get(
        getVoidType(ctx.llvmCtx),
        {getPtrType(ctx.llvmCtx)},
        false);
    llvm::Function* fn = llvm::Function::Create(
        fnType,
        llvm::GlobalValue::InternalLinkage,
        funcName,
        ctx.module);
    fn->getArg(0)->setName("inst");

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx.llvmCtx, "entry", fn);
    ctx.builder.SetInsertPoint(entry);
    ctx.setCurrentFunction(fn);

    // ─── Release fields in REVERSE declaration order ────────────────────
    // Reverse order matches "release in reverse of construction," which
    // matters when one field's resource was derived from an earlier
    // field's (e.g. `let b = a.someSlice()`).
    for (size_t i = layout.fields.size(); i > 0; --i) {
        ValueDeclAST* decl = layout.fields[i - 1];
        if (!decl->isa<VarDeclAST>()) continue;

        VarDeclAST* var = decl->as<VarDeclAST>();
        if (var->resourceKind == ResourceKind::None) continue;

        // Load the field's value, then emit its release.
        llvm::Type* fieldTy = layout.type->getElementType(i - 1);
        llvm::Value* fieldPtr = ctx.builder.CreateStructGEP(
            layout.type,
            fn->getArg(0),
            static_cast<unsigned>(i - 1),
            "free_field_" + ctx.pool.lookup(var->name));
        llvm::Value* value = ctx.builder.CreateLoad(fieldTy, fieldPtr,
                                                     "free_load");
        emitRelease(var, value, ctx);
    }

    ctx.builder.CreateRetVoid();
    ctx.setCurrentFunction(nullptr);
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
        llvm::Value* initValue = lowerExpression(info.init, ctx);
        if (initValue) {
            // Coerce fn → cls if the global's declared type is cls.
            initValue = maybeCoerceFnToCls(
                initValue,
                info.init->resolvedType,
                info.decl->type,   // the declared type of the global
                ctx);
            if (!initValue) continue;

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