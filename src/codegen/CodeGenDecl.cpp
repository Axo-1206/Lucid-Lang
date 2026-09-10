/// @file CodeGenDecl.cpp
/// @brief Code generation for declarations (variables, functions, structs, enums).
///
/// ─── Design Notes ─────────────────────────────────────────────────────────
/// This file lowers Lucid declarations to LLVM IR. It handles:
///   - Global variables (module-level `let`/`const`)
///   - Local variables (function-scope `let`/`const`)
///   - Function declarations (including foreign, generic, closure)
///   - Struct declarations (LLVM struct type creation)
///   - Enum declarations (LLVM integer type + variant constants)
///
/// ─── No Generic Substitution ─────────────────────────────────────────────
/// Sema handles ALL specialization. By the time a declaration reaches CodeGen:
///   - If it was specialized (default), it has concrete types and a mangledName
///   - If it was @[erased], it has erasedName and will be lowered as TaggedSlot

#include "CodeGen.hpp"
#include "types/CodeGenType.hpp"
#include "generic/CodeGenGeneric.hpp"
#include "support/CodeGenAlloca.hpp"
#include "support/CodeGenPanic.hpp"
#include "core/ASTStrings.hpp"
#include "core/trace/Trace.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

namespace codegen {

// =============================================================================
// Local Helpers
// =============================================================================

/// @brief Check if a local variable declaration owns a heap resource that
///        needs cleanup when its scope exits.
///
/// ─── Which Declarations Reach This Function ─────────────────────────────
/// Only `VarDeclAST`. Function-typed bindings are `FuncDeclAST`, not
/// `VarDeclAST` — they are declared with a `func_decl` (which carries a
/// `chain`), while `var_decl` only ever holds non-function types. So this
/// function never sees a function-typed binding at all.
///
/// Function-typed bindings (closures) are tracked alive in
/// `lowerFunctionDecl` / `lowerNormalFunctionDecl`, which handle the
/// `FuncDeclAST` side. That path is where the `let`-vs-`const` and
/// `hasClosure` decisions live.
///
/// ─── Rules for VarDeclAST ───────────────────────────────────────────────
///   - string:  always owned (heap-allocated UTF-8 buffer)  → cleanup
///   - [*]T:    always owned (heap-allocated dynamic array) → cleanup
///   - everything else (primitives, structs, fixed arrays,
///     pointers, references, nullable/fallible wrappers around
///     non-resource types):                                 → no cleanup
///
/// ─── Why This Is Just an Optimization, Not a Correctness Requirement ────
/// The cleanup machinery (`emitCleanupForTracker`, `ctx.reassign`) is
/// already self-guarding: every release is emitted only after checking the
/// resource type, and null pointers are skipped. Marking a non-resource
/// variable alive would not produce incorrect IR — it would just cause
/// extra no-op iterations at scope exit and reassignment. This helper
/// keeps the `alive` set small so those paths do only the work that
/// matters.
static bool needsScopeCleanup(VarDeclAST* decl) {
    if (!decl || !decl->type) return false;

    TypeAST* type = decl->type;

    // ─── Strings: always owned ──────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        return type->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::String;
    }

    // ─── Dynamic arrays: always owned ───────────────────────────────────
    if (type->isa<ArrayTypeAST>()) {
        return type->as<ArrayTypeAST>()->isDynamic();
    }

    // ─── Everything else: no cleanup ────────────────────────────────────
    // Note: function types cannot appear here — a function-typed binding
    // is a FuncDeclAST, not a VarDeclAST, and is handled separately in
    // lowerFunctionDecl/lowerNormalFunctionDecl.
    return false;
}

/// @brief Check if a declaration is exported, using the CodeGenContext's pool.
/// 
/// ─── Why This Takes the Context ─────────────────────────────────────────
/// `DeclAST` intentionally has no StringPool reference — it's a pure data
/// node. To compare an attribute name (an InternedString) against "export",
/// we need the same pool that interned the attribute names. The context
/// carries that pool.
static bool isExported(DeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return false;
    InternedString exportName = ctx.pool.intern("export");
    for (AttributeAST* attr : decl->attributes) {
        if (attr && attr->name == exportName) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// Struct Declaration
// =============================================================================

void lowerStructDecl(StructDeclAST* decl, CodeGenContext& ctx) {
    if (!decl || decl->hasSyntaxError) return;

    // ─── Check if Sema already generated the LLVM type ──────────────────
    if (decl->llvmType) {
        Trace::detail("Struct '", ctx.pool.lookup(decl->name),
                      "' already lowered, skipping");
        return;
    }

    // ─── Dispatch based on generic status ───────────────────────────────
    // If the struct still has genericParams, Sema chose the @[erased] path.
    // Otherwise, it's either non-generic or already specialized by Sema.
    if (isGenericStruct(decl)) {
        lowerGenericStructDecl(decl, ctx);
    } else {
        lowerNormalStructDecl(decl, ctx);
    }
}

void lowerGenericStructDecl(StructDeclAST* decl, CodeGenContext& ctx) {
    // ─── @[erased] path ─────────────────────────────────────────────────
    // Sema kept the template with genericParams intact and set erasedName.
    // Generate the type-erased version (fields become TaggedSlots).
    //
    // Empty type args: the erased struct representation is the same
    // regardless of what concrete types are instantiated.
    ArenaSpan<TypeAST*> emptyArgs{};
    llvm::Type* erasedType = getOrCreateInstantiatedStruct(decl, emptyArgs, ctx);
    if (!erasedType) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "failed to generate erased struct '",
                                ctx.pool.lookup(decl->name), "'");
        return;
    }
    Trace::detail("Lowered @[erased] struct '", ctx.pool.lookup(decl->name), "'");
}

void lowerNormalStructDecl(StructDeclAST* decl, CodeGenContext& ctx) {
    // ─── Specialized or non-generic path ────────────────────────────────
    // Sema created a specialized StructDeclAST with concrete field types
    // and a mangledName. Just create the LLVM struct type.
    llvm::StructType* structType = getStructType(ctx, decl);
    if (!structType) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "failed to create LLVM struct for '",
                                ctx.pool.lookup(decl->name), "'");
        return;
    }

    Trace::detail("Lowered struct '", ctx.pool.lookup(decl->name),
                  "' (", decl->fields.size(), " fields)");
}

// =============================================================================
// Enum Declaration
// =============================================================================

void lowerEnumDecl(EnumDeclAST* decl, CodeGenContext& ctx) {
    if (!decl || decl->hasSyntaxError) return;

    if (decl->backingLLVMType) {
        Trace::detail("Enum '", ctx.pool.lookup(decl->name),
                      "' already lowered, skipping");
        return;
    }

    // ─── 1. Get the backing integer type ────────────────────────────────
    llvm::IntegerType* backingType = getEnumType(ctx, decl);
    if (!backingType) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "failed to determine backing type for enum '",
                                ctx.pool.lookup(decl->name), "'");
        return;
    }
    decl->backingLLVMType = backingType;

    // ─── 2. Create LLVM constants for each variant ──────────────────────
    decl->variantConstants.clear();
    decl->variantConstants.reserve(decl->variants.size());

    for (EnumVariantAST* variant : decl->variants) {
        llvm::ConstantInt* constVal = llvm::ConstantInt::get(
            backingType,
            static_cast<uint64_t>(variant->value),
            /*isSigned=*/true
        );
        variant->llvmValue = constVal;
        decl->variantConstants.push_back(constVal);
    }

    // ─── 3. Store byte size for later use ───────────────────────────────
    const llvm::DataLayout& dl = ctx.module->getDataLayout();
    decl->byteSize = dl.getTypeAllocSize(backingType).getFixedValue();

    Trace::detail("Lowered enum '", ctx.pool.lookup(decl->name),
                  "' (", decl->variants.size(), " variants, ",
                  decl->byteSize, " bytes)");
}

// =============================================================================
// Function Declaration
// =============================================================================

void lowerFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl || decl->hasSyntaxError) return;

    // ─── Check if already lowered ───────────────────────────────────────
    if (ctx.lookupFunction(decl)) {
        Trace::detail("Function '", ctx.pool.lookup(decl->name),
                      "' already lowered, skipping");
        return;
    }

    // ─── Foreign functions: declare external symbol ─────────────────────
    if (decl->isForeignFunction) {
        lowerForeignFunctionDecl(decl, ctx);
        return;
    }

    // ─── Generic functions (@[erased]): generate erased prototype ───────
    if (isGenericFunction(decl)) {
        lowerGenericFunctionDecl(decl, ctx);
        return;
    }

    // ─── Normal functions (default): regular LLVM function ──────────────
    lowerNormalFunctionDecl(decl, ctx);
}

void lowerForeignFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    std::string symbolName = ctx.pool.lookup(decl->mangledName);
    if (symbolName.empty()) {
        symbolName = ctx.pool.lookup(decl->name);
    }

    // ─── Build the LLVM function type from the AST ──────────────────────
    llvm::FunctionType* fnType = getFunctionType(ctx, decl->funcType, false);
    if (!fnType) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "foreign function '", ctx.pool.lookup(decl->name),
                                "' has invalid signature");
        return;
    }

    // ─── Create external declaration (no body) ──────────────────────────
    llvm::Function* func = llvm::Function::Create(
        fnType,
        llvm::Function::ExternalLinkage,
        symbolName,
        ctx.module
    );

    decl->llvmFunction = func;
    ctx.storeFunction(decl, func);

    Trace::detail("Declared foreign function '", symbolName, "'");
}

void lowerGenericFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    // ─── @[erased] path ─────────────────────────────────────────────────
    // Sema kept the template with genericParams intact and set erasedName.
    // Generate the type-erased function prototype.
    llvm::Function* erasedFunc = generateErasedGenericFunction(decl, ctx);
    if (!erasedFunc) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "failed to generate erased function '",
                                ctx.pool.lookup(decl->name), "'");
        return;
    }
    decl->erasedFunction = erasedFunc;
    ctx.storeFunction(decl, erasedFunc);
    Trace::detail("Lowered @[erased] function '", ctx.pool.lookup(decl->name), "'");
}

void lowerNormalFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    // ─── 1. Get the mangled name ────────────────────────────────────────
    std::string funcName = ctx.pool.lookup(decl->mangledName);
    if (funcName.empty()) {
        funcName = ctx.pool.lookup(decl->name);
    }

    // ─── 2. Build the LLVM function type ────────────────────────────────
    bool hasClosure = decl->hasClosure;
    llvm::FunctionType* fnType = getFunctionType(ctx, decl->funcType, hasClosure);
    if (!fnType) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "function '", ctx.pool.lookup(decl->name),
                                "' has invalid signature");
        return;
    }

    // ─── 3. Determine linkage ───────────────────────────────────────────
    llvm::GlobalValue::LinkageTypes linkage = llvm::GlobalValue::InternalLinkage;
    if (isExported(decl, ctx)) {
        linkage = llvm::GlobalValue::ExternalLinkage;
    }

    // ─── 4. Create the LLVM function ────────────────────────────────────
    llvm::Function* func = llvm::Function::Create(
        fnType,
        linkage,
        funcName,
        ctx.module
    );
    decl->llvmFunction = func;
    ctx.storeFunction(decl, func);

    // ─── 5. Name the parameters for debugging ───────────────────────────
    size_t argIdx = 0;
    if (hasClosure) {
        func->getArg(argIdx++)->setName("env");
    }

    FuncTypeAST* paramTypeIter = decl->funcType;
    while (paramTypeIter) {
        for (ParamAST* param : paramTypeIter->params) {
            if (argIdx < func->arg_size()) {
                func->getArg(argIdx++)->setName(ctx.pool.lookup(param->name));
            }
        }
        paramTypeIter = paramTypeIter->getNext();
    }

    Trace::detail("Declared function '", funcName, "'");
}

// =============================================================================
// Function Body Lowering
// =============================================================================

void lowerFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl || decl->hasSyntaxError) return;

    // ─── Foreign functions have no body ─────────────────────────────────
    if (decl->isForeignFunction) return;

    // ─── Dispatch based on generic status ───────────────────────────────
    if (isGenericFunction(decl)) {
        lowerGenericFunctionBody(decl, ctx);
    } else {
        lowerNormalFunctionBody(decl, ctx);
    }
}

void lowerGenericFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    // ─── @[erased] path ─────────────────────────────────────────────────
    // The erased function prototype was created in lowerGenericFunctionDecl.
    // Now we generate its body with tagged-slot unpacking.
    llvm::Function* erasedFunc = decl->erasedFunction;
    if (!erasedFunc) {
        erasedFunc = ctx.lookupFunction(decl);
    }
    if (!erasedFunc) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "erased function '", ctx.pool.lookup(decl->name),
                                "' has no prototype");
        return;
    }
    lowerErasedFunctionBody(decl, erasedFunc, ctx);
}

void lowerNormalFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    llvm::Function* func = ctx.lookupFunction(decl);
    if (!func) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "function '", ctx.pool.lookup(decl->name),
                                "' has no prototype");
        return;
    }
    lowerFunctionBodyInternal(decl, func, ctx);
}

void lowerFunctionBodyInternal(FuncDeclAST* decl, llvm::Function* func, CodeGenContext& ctx) {
    // ─── Skip if function already has a body (e.g., forward decl) ───────
    if (!func->empty()) return;

    // ─── 1. Set up the entry block ──────────────────────────────────────
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(
        ctx.llvmCtx, "entry", func
    );
    ctx.builder.SetInsertPoint(entry);

    // ─── 2. Save the previous function context ──────────────────────────
    llvm::Function* prevFunc = ctx.currentFunction;
    llvm::Value* prevEnv = ctx.currentEnvPtr;
    ctx.currentFunction = func;

    // ─── 3. Push a live scope for the function body ─────────────────────
    ctx.pushLiveScope();

    // ─── 4. Handle closure environment ──────────────────────────────────
    if (decl->hasClosure) {
        ctx.currentEnvPtr = func->getArg(0);
    }

    // ─── 5. Allocate and store parameters ───────────────────────────────
    size_t argIdx = decl->hasClosure ? 1 : 0;
    FuncTypeAST* paramTypeIter = decl->funcType;
    while (paramTypeIter) {
        for (ParamAST* param : paramTypeIter->params) {
            if (argIdx >= func->arg_size()) break;

            llvm::Value* arg = func->getArg(argIdx++);
            llvm::Type* paramType = getType(ctx, param->type);
            if (paramType) {
                llvm::AllocaInst* alloca = ctx.builder.CreateAlloca(
                    paramType, nullptr, ctx.pool.lookup(param->name)
                );
                ctx.builder.CreateStore(arg, alloca);
                param->llvmAlloca = alloca;
                ctx.storeValue(param, alloca);
            }
        }
        paramTypeIter = paramTypeIter->getNext();
    }

    // ─── 6. Lower the body statements ───────────────────────────────────
    if (decl->body) {
        if (decl->body->isa<BlockStmtAST>()) {
            lowerBlockStmt(decl->body->as<BlockStmtAST>(), ctx);
        } else if (decl->body->isa<ReturnStmtAST>()) {
            lowerReturnStmt(decl->body->as<ReturnStmtAST>(), ctx);
        }
    }

    // ─── 7. Ensure a terminator exists (void functions) ─────────────────
    if (!ctx.builder.GetInsertBlock()->getTerminator()) {
        if (decl->funcType->returnType) {
            llvm::Type* retType = getType(ctx, decl->funcType->returnType);
            if (retType) {
                ctx.builder.CreateRet(llvm::UndefValue::get(retType));
            } else {
                ctx.builder.CreateRetVoid();
            }
        } else {
            ctx.builder.CreateRetVoid();
        }
    }

    // ─── 8. Pop the function scope (emits cleanup) ──────────────────────
    ctx.popLiveScope();

    // ─── 9. Restore the previous function context ───────────────────────
    ctx.currentFunction = prevFunc;
    ctx.currentEnvPtr = prevEnv;

    Trace::detail("Lowered body of function '", ctx.pool.lookup(decl->name), "'");
}

// =============================================================================
// Variable Declarations
// =============================================================================

void lowerVarDecl(VarDeclAST* decl, CodeGenContext& ctx) {
    if (!decl || decl->hasSyntaxError) return;

    llvm::Type* varType = getType(ctx, decl->type);
    if (!varType) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "variable '", ctx.pool.lookup(decl->name),
                                "' has unknown type");
        return;
    }

    // ─── Dispatch: module-level vs local ────────────────────────────────
    bool isModuleLevel = !ctx.currentFunction;
    if (isModuleLevel) {
        lowerGlobalVar(decl, varType, ctx);
    } else {
        lowerLocalVar(decl, varType, ctx);
    }
}

void lowerGlobalVar(VarDeclAST* decl, llvm::Type* varType, CodeGenContext& ctx) {
    if (!decl->mangledName.isValid()) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "global variable '", ctx.pool.lookup(decl->name),
                                "' has no mangled name (Sema should have set this)");
        return;
    }

    std::string name = ctx.pool.lookup(decl->mangledName);

    // ─── Determine linkage ──────────────────────────────────────────────
    llvm::GlobalValue::LinkageTypes linkage = llvm::GlobalValue::InternalLinkage;
    if (isExported(decl, ctx)) {
        linkage = llvm::GlobalValue::ExternalLinkage;
    }

    // ─── Create the global with a zero initializer ──────────────────────
    // Runtime initialization is deferred to __init_globals for non-constant
    // initializers.
    llvm::Constant* zeroInit = llvm::Constant::getNullValue(varType);
    llvm::GlobalVariable* global = new llvm::GlobalVariable(
        *ctx.module,
        varType,
        decl->isConst(),
        linkage,
        zeroInit,
        name
    );
    decl->llvmGlobal = global;
    ctx.storeValue(decl, global);

    // ─── Queue for runtime initialization if needed ─────────────────────
    if (decl->init) {
        ctx.pendingGlobals.push_back({
            decl,
            decl->init,
            global,
            ctx.currentModule,
            decl->orderInModule
        });
        Trace::detail("Global '", ctx.pool.lookup(decl->name),
                      "' queued for runtime initialization");
    }
}

void lowerLocalVar(VarDeclAST* decl, llvm::Type* varType, CodeGenContext& ctx) {
    // ─── 1. Allocate the variable on the stack ──────────────────────────
    llvm::AllocaInst* alloca = ctx.builder.CreateAlloca(
        varType,
        nullptr,
        ctx.pool.lookup(decl->name)
    );
    decl->llvmAlloca = alloca;
    ctx.storeValue(decl, alloca);

    // ─── 2. Track as a live variable for scope cleanup ──────────────────
    // Only variables that actually own a heap resource are tracked. The
    // decision consults the declaration (not just the type) so that
    // function-typed bindings are only tracked when they can actually hold
    // a closure:
    //   - `let`-bound functions      → tracked (may be reassigned)
    //   - `const`-bound named fns    → tracked iff the fn captures
    //   - `const`-bound anon fns     → tracked iff the fn captures
    //
    // The cleanup machinery (`emitCleanupForTracker`, `ctx.reassign`) is
    // self-guarding, so tracking a non-resource variable would not produce
    // incorrect IR — it would just cause extra no-op iterations at scope
    // exit and reassignment. This helper keeps the `alive` set minimal.
    if (needsScopeCleanup(decl)) {
        ctx.markAlive(decl);
    }

    // ─── 3. Evaluate initializer (if any) ───────────────────────────────
    if (decl->init) {
        llvm::Value* initValue = lowerExpression(decl->init, ctx);
        if (!initValue) {
            ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                    "failed to lower initializer for '",
                                    ctx.pool.lookup(decl->name), "'");
            return;
        }

        // ─── Bitcast if pointer types differ (opaque pointer safety) ────
        if (initValue->getType() != varType) {
            if (initValue->getType()->isPointerTy() && varType->isPointerTy()) {
                initValue = ctx.builder.CreateBitCast(initValue, varType);
            }
        }

        ctx.builder.CreateStore(initValue, alloca);
    }

    Trace::detail("Lowered local var '", ctx.pool.lookup(decl->name), "'");
}

// =============================================================================
// Main Declaration Dispatch
// =============================================================================

void lowerDeclaration(DeclAST* decl, CodeGenContext& ctx) {
    if (!decl || decl->hasSyntaxError) return;

    switch (decl->kind) {
        case ASTKind::FuncDecl:
            lowerFunctionDecl(decl->as<FuncDeclAST>(), ctx);
            break;
        case ASTKind::VarDecl:
            lowerVarDecl(decl->as<VarDeclAST>(), ctx);
            break;
        case ASTKind::StructDecl:
            lowerStructDecl(decl->as<StructDeclAST>(), ctx);
            break;
        case ASTKind::EnumDecl:
            lowerEnumDecl(decl->as<EnumDeclAST>(), ctx);
            break;
        case ASTKind::TraitDecl:
            // Traits are compile-time only — no runtime representation.
            break;
        case ASTKind::ImportDecl:
            // Imports are handled by the module system — nothing to lower.
            break;
        default:
            // Unknown declaration kind — Sema should have rejected it.
            break;
    }
}

} // namespace codegen