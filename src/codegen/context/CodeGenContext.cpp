/// @file CodeGenContext.cpp
/// @brief Implementation of CodeGenContext methods

#include "CodeGenContext.hpp"
#include "../intrinsic/LucidIntrinsicEmitter.hpp"
#include "../types/CodeGenType.hpp"
#include "codegen/memory/CodeGenOwnership.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/GlobalVariable.h>

namespace codegen {

// ─── Runtime Function Helpers ─────────────────────────────────────────────

llvm::Function* CodeGenContext::getOrCreateRuntimeFunction(const std::string& name, llvm::FunctionType* type) {
    llvm::Function* func = getRuntimeFunction(name);
    if (func) return func;

    func = llvm::Function::Create(
        type,
        llvm::Function::ExternalLinkage,
        name,
        module
    );
    setRuntimeFunction(name, func);
    return func;
}

llvm::Function* CodeGenContext::getRuntimeFn(RuntimeFn fn) {
    const RuntimeFunctionInfo& info = getRuntimeFunctionInfo(fn);
    std::string name(info.name);
    
    llvm::Function* func = getRuntimeFunction(name);
    if (func) return func;
    
    llvm::FunctionType* type = info.buildType(*this);
    
    func = llvm::Function::Create(
        type,
        llvm::Function::ExternalLinkage,
        name,
        module
    );
    
    setRuntimeFunction(name, func);
    return func;
}

llvm::Function* CodeGenContext::getOrInsertFunction(const std::string& name, llvm::FunctionType* type) {
    llvm::FunctionCallee callee = module->getOrInsertFunction(name, type);
    return llvm::dyn_cast<llvm::Function>(callee.getCallee());
}

// ─── Module Layout ─────────────────────────────────────────────

ModuleInstanceLayout& CodeGenContext::getOrCreateModuleLayout(ModuleAST* module) {
    auto it = moduleLayouts.find(module);
    if (it != moduleLayouts.end()) {
        return it->second;
    }
    // getModuleInstanceType populates the layout as a side effect.
    getModuleInstanceType(*this, module);
    return moduleLayouts[module];
}

// ─── Module Instance Table ──────────────────────────────────────────────

llvm::GlobalVariable* CodeGenContext::getOrDeclareModuleTable() {
    const size_t N = modules.size() ? modules.size() : 1;
    llvm::ArrayType* tableType =
        llvm::ArrayType::get(getPtrType(llvmCtx), N);

    if (llvm::GlobalVariable* existing = module->getGlobalVariable("__lucid_module_instances", /*AllowInternal=*/true)) {
        return existing;
    }

    // Declare it. If we're the first module, CodeGen.cpp already created
    // the definition (see emitModuleInstanceTable); this path only fires
    // for non-first modules, creating a declaration.
    return new llvm::GlobalVariable(
        *module,
        tableType,
        /*isConstant=*/false,
        llvm::GlobalValue::ExternalLinkage,
        /*Initializer=*/nullptr,   // declaration only
        "__lucid_module_instances");
}

llvm::Value* CodeGenContext::loadModuleInstance(ModuleAST* target) {
    uint32_t id = moduleId(target);
    assert(id != UINT32_MAX && "loadModuleInstance: unknown module");

    llvm::GlobalVariable* table = getOrDeclareModuleTable();
    llvm::Value* slot = builder.CreateConstInBoundsGEP2_64(
        table->getValueType(),   // [N x ptr]
        table,
        0,
        static_cast<uint64_t>(id),
        "module_slot");
    return builder.CreateLoad(getPtrType(llvmCtx), slot, "module_inst");
}

// ─── Live Variable Helpers ────────────────────────────────────────────────

void CodeGenContext::emitCleanupForTracker(const LiveVariableTracker& tracker) {
    if (!getCurrentFunction()) return;

    // ─── Phase 1: user #scope_exit callbacks ──────────────────────────
    if (tracker.block) {
        for (size_t i = tracker.block->scopeExits.size(); i > 0; --i) {
            const ScopeExitRegistration* reg = tracker.block->scopeExits[i - 1];
            emitScopeExitCallback(reg, *this);
        }
    }

    // ─── Phase 2: implicit cleanup ──────────────────────────────────────
    // Read-only w.r.t. `tracker` — see the note in the header. All
    // resource-kind dispatch lives in emitRelease now; this function just
    // walks the alive set and hands each binding's current value over.
    std::vector<ValueDeclAST*> declarations = tracker.getAliveVariables();

    auto loadValue = [&](llvm::Value* val) -> llvm::Value* {
        if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(val)) {
            return builder.CreateLoad(alloca->getAllocatedType(), alloca,
                                      "cleanup_load");
        }
        return val;
    };

    for (ValueDeclAST* decl : declarations) {
        llvm::Value* binding = lookupValue(decl);
        if (!binding || !decl->type) continue;

        llvm::Value* value = loadValue(binding);
        emitRelease(decl, value, *this);
    }
}

void CodeGenContext::emitUnwindTo(size_t targetDepth) {
    if (!getCurrentFunction()) {
        return;
    }

    // ─── Guard against invalid target depth ──────────────────────────────
    if (targetDepth >= liveTrackers.size()) {
        return;
    }

    // ─── Unwind scopes ────────────────────────────────────────────────────
    // Non-destructive: emit cleanup for each scope from innermost down to
    // (but not including) targetDepth, using a snapshot of whatever is
    // currently alive in it — but do NOT pop or mutate liveTrackers. This
    // is one divergent exit edge (the return/break/continue statement that
    // called us); it does not own these scopes' lifetimes. Only each
    // tracker's own structurally-paired popLiveScope() call — reached when
    // its owning lowerBlockStmt/lowerFunctionBody/loop frame actually
    // finishes — may remove it from the stack. See the doc comment on this
    // function's declaration in CodeGenContext.hpp for the full rationale.
    for (size_t i = liveTrackers.size(); i > targetDepth; --i) {
        emitCleanupForTracker(liveTrackers[i - 1]);
    }
}

// ─── String Literal Helper ───────────────────────────────────────────────

llvm::Value* CodeGenContext::createStringLiteral(const std::string& str) {
    llvm::Constant* strConst = llvm::ConstantDataArray::getString(llvmCtx, str);
    llvm::GlobalVariable* global = new llvm::GlobalVariable(
        *module,
        strConst->getType(),
        true,
        llvm::GlobalValue::PrivateLinkage,
        strConst
    );

    llvm::Type* strType = getStringType();
    llvm::Type* i64 = llvm::Type::getInt64Ty(llvmCtx);
    llvm::Type* i8Ptr = llvm::PointerType::get(llvmCtx, 0);

    llvm::Value* ptr = builder.CreateBitCast(global, i8Ptr);
    llvm::Value* len = llvm::ConstantInt::get(i64, str.length());

    llvm::Value* result = llvm::UndefValue::get(strType);
    result = builder.CreateInsertValue(result, ptr, 0);
    result = builder.CreateInsertValue(result, len, 1);
    result = builder.CreateInsertValue(result, len, 2);
    return result;
}

// ─── Intrinsic Helpers ─────────────────────────────────────────────────────

llvm::Function* CodeGenContext::getLLVMIntrinsicDecl(llvm::Intrinsic::ID id, llvm::ArrayRef<llvm::Type*> argTypes) {
    return llvm::Intrinsic::getDeclaration(module, id, argTypes);
}

// ─── Pointee Type Helpers ─────────────────────────────────────────────────

llvm::Type* CodeGenContext::getPointeeType(llvm::Value* ptr) const {
    (void)ptr;
    return llvm::Type::getInt8Ty(llvmCtx);
}

llvm::Type* CodeGenContext::getPointeeType(llvm::Type* type) const {
    (void)type;
    return llvm::Type::getInt8Ty(llvmCtx);
}

// ─── reassign ───────────────────────────────────────────────────────────────

void CodeGenContext::reassign(ValueDeclAST* decl, llvm::Value* oldValue,
                              llvm::Value* newValue) {
    if (!decl || !oldValue || !newValue) return;
    if (liveTrackers.empty()) return;

    if (!isAlive(decl)) return;  // Nothing to clean up

    // ─── Reject linear types before doing anything ─────────────────────
    // Future<T> and Thread<T> cannot be reassigned while pending/running.
    // This is a semantic check, not a resource-kind check, so it lives
    // here rather than in classifyResource.
    TypeAST* type = decl->type;
    if (!type) return;

    if (type->isa<FutureTypeAST>()) {
        diagnostics.errorAt(DiagCode::Sem_InvalidUnary, decl->loc,
                            "internal error: Future<T> cannot be reassigned "
                            "while pending");
        return;
    }
    if (type->isa<ThreadTypeAST>()) {
        diagnostics.errorAt(DiagCode::Sem_InvalidUnary, decl->loc,
                            "internal error: Thread<T> cannot be reassigned "
                            "while running");
        return;
    }

    // ─── Release the old resource ──────────────────────────────────────
    // The binding stays alive; only the old value's claim is dropped.
    // emitRelease normalizes alloca→value itself, but oldValue here is
    // already a loaded value (lowerAssignExpr loads it before calling).
    emitRelease(decl, oldValue, *this);

    // newValue is intentionally unused by this function — the caller
    // (lowerAssignExpr) is responsible for the retain-on-copy decision,
    // because it's the one that knows whether the RHS was a fresh literal
    // or an existing binding (Rule 1 vs Rule 2). See the ownership model
    // header.
    (void)newValue;
}

} // namespace codegen