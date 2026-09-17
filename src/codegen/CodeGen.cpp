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
    llvm::LLVMContext& context,
    const CodeGenOptions& options
) {
    std::vector<std::unique_ptr<llvm::Module>> result;
    result.reserve(modules.size());

    CodeGenContext ctx(p, d, context);
    ctx.options = options;
    ctx.modules = modules;

    // ─── Module ID assignment ────────────────────────────────────────────
    if (options.moduleIds) {
        ctx.moduleIds = *options.moduleIds;
    } else {
        for (size_t i = 0; i < modules.size(); ++i) {
            if (modules[i]) ctx.moduleIds[modules[i]] = static_cast<uint32_t>(i);
        }
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

        // Pre-declare @__lucid_module_instances in every module
        ctx.getOrDeclareModuleTable();

        generateModule(module, ctx);
        result.push_back(std::unique_ptr<llvm::Module>(ctx.module));
    }

    // ─── Phase 2: Per-module __module_size_<name> / __init_module_<name> / __free_module_<name> ─
    //
    // Emitted after the per-module loop, because they lower initializer
    // expressions that may reference symbols from other modules (already
    // lowered in Phase 1).
    for (ModuleAST* module : modules) {
        if (!module) continue;

        // Find the llvm::Module we created for this ModuleAST.
        llvm::Module* savedModule = ctx.module;
        ModuleAST* savedCurrentModule = ctx.currentModule;

        ctx.module = ctx.llvmModules[module];
        ctx.currentModule = module;

        generateModuleSize(module, ctx);
        generateModuleInit(module, ctx);
        generateModuleFree(module, ctx);

        ctx.module = savedModule;
        ctx.currentModule = savedCurrentModule;
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

void generateModuleSize(ModuleAST* module, CodeGenContext& ctx) {
    if (!module || !ctx.module) return;

    std::string funcName =
        "__module_size_" + sanitizeForLLVMSymbol(ctx.pool.lookup(module->filePath));

    llvm::FunctionType* fnType = llvm::FunctionType::get(
        getI64Type(ctx.llvmCtx),
        false);
    llvm::Function* fn = llvm::Function::Create(
        fnType,
        llvm::GlobalValue::ExternalLinkage,
        funcName,
        ctx.module);

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx.llvmCtx, "entry", fn);
    ctx.builder.SetInsertPoint(entry);

    uint64_t sz = 0;
    llvm::StructType* instTy = getModuleInstanceType(ctx, module);
    if (instTy && instTy->isSized()) {
        sz = ctx.module->getDataLayout()
                  .getTypeAllocSize(instTy).getFixedValue();
    }

    ctx.builder.CreateRet(llvm::ConstantInt::get(getI64Type(ctx.llvmCtx), sz));
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
        llvm::GlobalValue::ExternalLinkage,
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

        // Match the module-init coercion handling.
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
        llvm::GlobalValue::ExternalLinkage,
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
    // A non-generic VarDeclAST is intentionally a no-op at declaration time:
    // its storage sits in the module instance layout, while initialization and
    // cleanup are emitted by the per-module __init_module_<name> and
    // __free_module_<name> helpers.
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

} // namespace codegen