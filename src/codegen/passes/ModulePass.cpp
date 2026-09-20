/// @file codegen/passes/ModulePass.cpp
/// @brief Module state globals and program-level initialization.

#include "Passes.hpp"

#include "codegen/Emitter.hpp"
#include "codegen/Program.hpp"
#include "codegen/Types.hpp"

#include "core/ast/DeclAST.hpp"
#include "core/ast/ModuleAST.hpp"
#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

#include <cassert>

namespace codegen {

namespace {

/// Emit `@__module_state_<id>` as a global of the module's instance type.
llvm::GlobalVariable* emitModuleStateGlobal(
    ModuleAST* module,
    ProgramState& program,
    ModuleInstanceLayout& layout)
{
    if (!layout.type) return nullptr;

    std::string name = "__module_state_"
                     + sanitizeForSymbol(program.pool.lookup(module->filePath));

    llvm::GlobalVariable* existing = program.module().getGlobalVariable(
        name, /*AllowInternal=*/true);
    if (existing) return existing;

    return new llvm::GlobalVariable(
        program.module(),
        layout.type,
        /*isConstant=*/false,
        llvm::GlobalValue::ExternalLinkage,
        llvm::Constant::getNullValue(layout.type),  // zero-initialized
        name);
}

/// Emit `__module_size_<name>() -> i64`.
void emitModuleSize(ModuleAST* module,
                    ProgramState& program,
                    ModuleInstanceLayout& layout) {
    std::string name = "__module_size_"
                     + sanitizeForSymbol(program.pool.lookup(module->filePath));

    if (program.module().getFunction(name)) return;

    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getInt64Ty(ctx), {}, false);
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::ExternalLinkage, name,
        program.module());

    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(ctx, "entry", fn);
    llvm::IRBuilder<> b(entry);

    uint64_t size = 0;
    if (layout.type && layout.type->isSized()) {
        size = program.module().getDataLayout()
                   .getTypeAllocSize(layout.type).getFixedValue();
    }
    b.CreateRet(llvm::ConstantInt::get(
        llvm::Type::getInt64Ty(ctx), size));
}

/// Emit `__init_module_<name>(ptr %inst) -> void`.
///
/// For each field in declaration order, emit the initializer expression
/// and store the result into the instance field.
void emitModuleInit(ModuleAST* module,
                    ProgramState& program,
                    Emitter& emitter,
                    ModuleInstanceLayout& layout) {
    if (layout.fields.empty()) return;

    std::string name = "__init_module_"
                     + sanitizeForSymbol(program.pool.lookup(module->filePath));
    if (program.module().getFunction(name)) return;

    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::ExternalLinkage, name,
        program.module());
    fn->getArg(0)->setName("inst");

    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(ctx, "entry", fn);
    program.builder().SetInsertPoint(entry);

    // Enter a synthetic function state so the emitter's helpers work.
    // There's no `llvm::Function*` associated with a module initializer
    // as a "function body" in the sense the emitter cares about — but
    // we need a FunctionState for the emitter's scope stack.
    //
    // The module initializer's body is a sequence of stores; no locals,
    // no returns. It doesn't need a scope stack in practice, but the
    // emitter assumes one exists.
    //
    // Approach: construct a FunctionState around the body, run the
    // stores, destruct.
    program.setCurrentFunctionState(fn, nullptr);

    for (size_t i = 0; i < layout.fields.size(); ++i) {
        ValueDeclAST* decl = layout.fields[i];
        if (!decl) continue;

        if (!decl->isa<VarDeclAST>()) continue;  // reserved slots
        VarDeclAST* var = decl->as<VarDeclAST>();

        if (!var->init) continue;

        // Emit the initializer.
        Val initVal = emitter.emit(var->init);
        if (!initVal.isValid()) continue;

        // Coerce to the field type.
        initVal = emitter.coerceTo(initVal, var->type);
        if (!initVal.isValid()) continue;

        // GEP to the field.
        llvm::Value* fieldPtr = program.builder().CreateStructGEP(
            layout.type, fn->getArg(0), static_cast<unsigned>(i),
            "init_field_" + program.pool.lookup(var->name));

        // Acquire a fresh claim (intoOwned) and store.
        Val owned = program.ownership().intoOwned(
            initVal, program.builder());
        program.builder().CreateStore(owned.v, fieldPtr);
    }

    program.builder().CreateRetVoid();
    program.clearCurrentFunctionState();
}

/// Emit `__free_module_<name>(ptr %inst) -> void`.
///
/// For each field in reverse declaration order, load the field, drop
/// its resource, and (optionally) zero the field to prevent double-drop.
void emitModuleFree(ModuleAST* module,
                    ProgramState& program,
                    ModuleInstanceLayout& layout) {
    if (layout.fields.empty()) return;

    std::string name = "__free_module_"
                     + sanitizeForSymbol(program.pool.lookup(module->filePath));
    if (program.module().getFunction(name)) return;

    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), {ptrTy}, false);
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::ExternalLinkage, name,
        program.module());
    fn->getArg(0)->setName("inst");

    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(ctx, "entry", fn);
    program.builder().SetInsertPoint(entry);

    program.setCurrentFunctionState(fn, nullptr);

    // Reverse iteration.
    for (size_t i = layout.fields.size(); i > 0; --i) {
        ValueDeclAST* decl = layout.fields[i - 1];
        if (!decl) continue;
        if (decl->resourceKind == ResourceKind::None) continue;

        llvm::Type* fieldTy = layout.type->getElementType(i - 1);
        llvm::Value* fieldPtr = program.builder().CreateStructGEP(
            layout.type, fn->getArg(0), static_cast<unsigned>(i - 1),
            "free_field_" + program.pool.lookup(decl->name));

        llvm::Value* value = program.builder().CreateLoad(
            fieldTy, fieldPtr, "free_load");

        program.ownership().drop(decl->type, value, program.builder());
    }

    program.builder().CreateRetVoid();
    program.clearCurrentFunctionState();
}

/// Emit `__lucid_program_init() -> void` that calls each module's
/// `__init` in dependency order.
void emitProgramInit(const std::vector<ModuleAST*>& modules,
                     ProgramState& program,
                     const std::vector<std::string>& initSymbols) {
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), {}, false);
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::ExternalLinkage,
        "__lucid_program_init", program.module());

    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(ctx, "entry", fn);
    program.builder().SetInsertPoint(entry);

    for (size_t i = 0; i < modules.size(); ++i) {
        if (!modules[i]) continue;
        if (initSymbols[i].empty()) continue;

        // Look up the module's state global.
        ModuleInstanceLayout& layout =
            program.moduleLayouts()[modules[i]];
        if (!layout.type) continue;

        std::string stateName = "__module_state_"
                              + sanitizeForSymbol(
                                    program.pool.lookup(modules[i]->filePath));
        llvm::GlobalVariable* state =
            program.module().getGlobalVariable(stateName, true);
        if (!state) continue;

        // Look up the init function.
        llvm::Function* initFn = program.module().getFunction(initSymbols[i]);
        if (!initFn) continue;

        // Call init(state).
        program.builder().CreateCall(initFn, {state});
    }

    program.builder().CreateRetVoid();
}

/// Emit `__lucid_program_free() -> void` that calls each module's
/// `__free` in reverse dependency order.
void emitProgramFree(const std::vector<ModuleAST*>& modules,
                     ProgramState& program,
                     const std::vector<std::string>& freeSymbols) {
    llvm::LLVMContext& ctx = program.llvmContext();
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(ctx), {}, false);
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::ExternalLinkage,
        "__lucid_program_free", program.module());

    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(ctx, "entry", fn);
    program.builder().SetInsertPoint(entry);

    for (size_t i = modules.size(); i > 0; --i) {
        size_t idx = i - 1;
        if (!modules[idx]) continue;
        if (freeSymbols[idx].empty()) continue;

        ModuleInstanceLayout& layout =
            program.moduleLayouts()[modules[idx]];
        if (!layout.type) continue;

        std::string stateName = "__module_state_"
                              + sanitizeForSymbol(
                                    program.pool.lookup(modules[idx]->filePath));
        llvm::GlobalVariable* state =
            program.module().getGlobalVariable(stateName, true);
        if (!state) continue;

        llvm::Function* freeFn = program.module().getFunction(freeSymbols[idx]);
        if (!freeFn) continue;

        program.builder().CreateCall(freeFn, {state});
    }

    program.builder().CreateRetVoid();
}

} // anonymous namespace

void runModulePass(const std::vector<ModuleAST*>& modules,
                   ProgramState& program,
                   Manifest& manifest) {
    Trace::info("ModulePass: ", modules.size(), " modules");

    Emitter& emitter = program.emitter();
    std::vector<std::string> initSymbols(modules.size());
    std::vector<std::string> freeSymbols(modules.size());

    // ─── Dependency order check ───────────────────────────────────────────
    // The modules list is supposed to be in dependency order: a module's
    // dependencies appear before it. Verify by checking each module's
    // resolved imports against the set of modules seen so far.
    std::set<ModuleAST*> seenSoFar;
    for (ModuleAST* module : modules) {
        if (!module) continue;
        for (const auto& [alias, dep] : module->resolvedImports) {
            if (dep && !seenSoFar.count(dep)) {
                program.diagnostics.errorAt(
                    DiagCode::Backend_CodegenError, module->loc,
                    "module '", program.pool.lookup(module->filePath),
                    "' depends on '", program.pool.lookup(dep->filePath),
                    "' which appears later in the module list — "
                    "ModuleResolver produced an incorrect dependency order");
            }
        }
        seenSoFar.insert(module);
    }

    // ─── Emit per-module state and helpers ────────────────────────────────
    for (size_t i = 0; i < modules.size(); ++i) {
        ModuleAST* module = modules[i];
        if (!module) continue;

        program.currentModule = module;
        ModuleInstanceLayout& layout = program.moduleLayouts()[module];

        // Emit the state global.
        emitModuleStateGlobal(module, program, layout);

        // Emit the three helpers.
        emitModuleSize(module, program, layout);
        emitModuleInit(module, program, emitter, layout);
        emitModuleFree(module, program, layout);

        // Record the symbol names in the manifest (overwriting what
        // DeclarePass wrote, if anything changed).
        std::string sanitized = sanitizeForSymbol(
            program.pool.lookup(module->filePath));
        initSymbols[i] = "__init_module_" + sanitized;
        freeSymbols[i] = "__free_module_" + sanitized;
    }

    // ─── Emit program-level init and free ─────────────────────────────────
    emitProgramInit(modules, program, initSymbols);
    emitProgramFree(modules, program, freeSymbols);

    program.currentModule = nullptr;

    // ─── Populate manifest ────────────────────────────────────────────────
    manifest.programInitSymbol = "__lucid_program_init";
    manifest.programFreeSymbol = "__lucid_program_free";

    // Runtime symbols — populated by Abi's used-functions set.
    for (RuntimeFn fn : program.usedRuntimeFns()) {
        ManifestRuntimeSymbol sym;
        sym.symbol = std::string(
            getRuntimeFunctionInfo(fn).name);  // or via a lookup helper
        sym.definedByModule = false;
        manifest.runtimeSymbols.push_back(std::move(sym));
    }

    Trace::detail("ModulePass complete");
}

} // namespace codegen