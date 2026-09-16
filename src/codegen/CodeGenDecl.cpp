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

#include "CodeGen.hpp"
#include "codegen/runtime/closure/CodeGenClosure.hpp"
#include "memory/CodeGenOwnership.hpp"
#include "types/CodeGenType.hpp"
#include "memory/CodeGenAlloca.hpp"
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

/// True iff this declaration's init is an anonymous function expression
/// (with or without captures). Used to decide whether the body needs
/// lowering at all — a reference body has no body of its own to lower.
static AnonFuncExprAST* anonInit(FuncDeclAST* decl) {
    if (!decl->init || !decl->init->isa<AnonFuncExprAST>()) return nullptr;
    return decl->init->as<AnonFuncExprAST>();
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

    AST_ASSERT_MSG(!decl->isGeneric(),
        "lowerStructDecl received a generic template — "
        "lowerDeclaration should have skipped it.");


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

    // ─── 0. Idempotency guard ───────────────────────────────────────────
    // If already lowered, skip. For a non-capturing function, ctx.functions
    // holds the bare prototype; for a capturing function, ctx.values holds
    // the closure fat pointer. Check both so the guard fires in either case.
    if (ctx.lookupFunction(decl) || ctx.hasValue(decl)) {
        Trace::detail("Function '", ctx.pool.lookup(decl->name),
                      "' already lowered, skipping");
        return;
    }

    AST_ASSERT_MSG(!decl->isGeneric(),
        "lowerFunctionDecl received a generic template — "
        "lowerDeclaration should have skipped it. A generic template "
        "has no LLVM function; only its specializations do.");

    // ─── 1. Foreign functions: declare external symbol ──────────────────
    if (decl->isForeignFunction) {
        lowerForeignFunctionDecl(decl, ctx);
        return;
    }

    // ─── 2. cls-shaped function: lower as a closure value, not a bare fn ─
    // A cls-shaped declaration is a runtime fat pointer: either a block-body
    // closure lowered with lowerClosure, or a reference-body closure whose init
    // is already the closure value. The runtime shape decides the lowering, not
    // the presence of a capture flag on the anonymous expression.
    FuncShape shape = decl->funcType ? decl->funcType->shape : FuncShape::Fn;
    if (shape == FuncShape::Cls) {
        llvm::Value* closureValue = nullptr;
        AnonFuncExprAST* anon = anonInit(decl);

        if (anon) {
            closureValue = lowerClosure(anon, ctx);
        } else if (decl->init) {
            closureValue = lowerExpression(decl->init, ctx);
        } else {
            ctx.diagnostics.errorAt(DiagCode::Sem_MissingFuncBody, decl->loc,
                "cls-declared function '", ctx.pool.lookup(decl->name),
                "' has no body or initializer");
            return;
        }

        if (!closureValue) return;
        ctx.storeValue(decl, closureValue);

        Trace::detail("Lowered cls-shaped function '",
                      ctx.pool.lookup(decl->name),
                      "' as a closure value");
        return;
    }

    // ─── 3. Non-capturing: bare-function path ───────────────────────────

    // ─── 3.1. Get the mangled name ──────────────────────────────────────
    std::string funcName = ctx.pool.lookup(decl->mangledName);
    if (funcName.empty()) {
        funcName = ctx.pool.lookup(decl->name);
    }

    // ─── 3.2. Build the LLVM function type ──────────────────────────────
    llvm::FunctionType* fnType = getFunctionType(ctx, decl->funcType, false);
    if (!fnType) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "function '", ctx.pool.lookup(decl->name),
                                "' has invalid signature");
        return;
    }

    // ─── 3.3. Determine linkage ─────────────────────────────────────────
    llvm::GlobalValue::LinkageTypes linkage = llvm::GlobalValue::InternalLinkage;
    if (isExported(decl, ctx)) {
        linkage = llvm::GlobalValue::ExternalLinkage;
    }

    // ─── 3.4. Create the LLVM function ──────────────────────────────────
    llvm::Function* func = llvm::Function::Create(
        fnType,
        linkage,
        funcName,
        ctx.module
    );
    decl->llvmFunction = func;
    ctx.storeFunction(decl, func);

    // ─── 3.5. Name the parameters for debugging ─────────────────────────
    size_t argIdx = 0;
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

// =============================================================================
// Function Body Lowering
// =============================================================================

void lowerFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl || decl->hasSyntaxError) return;

    // ─── 1. Foreign functions have no body ──────────────────────────────
    if (decl->isForeignFunction) return;

    // ─── 2. cls-shaped functions: body was already lowered as part of the
    // closure value construction (block-body via lowerClosure, reference-body
    // by storing the reference as the closure value itself).
    FuncShape shape = decl->funcType ? decl->funcType->shape : FuncShape::Fn;
    if (shape == FuncShape::Cls) {
        Trace::detail("Skipping body lowering for cls-shaped function '",
                      ctx.pool.lookup(decl->name),
                      "' (body lowered during closure lowering)");
        return;
    }

    // ─── 3. Reference body: nothing to lower ────────────────────────────
    // If the init is not an AnonFuncExprAST, the function's body is a
    // reference to another function or a call. No body IR belongs to 
    // this declaration — the referenced function has its
    // own body already lowered where it was declared.
    AnonFuncExprAST* anon = anonInit(decl);
    if (!anon) {
        Trace::detail("Skipping body lowering for reference-body function '",
                      ctx.pool.lookup(decl->name), "'");
        return;
    }

    // ─── 4. Get the LLVM function prototype ─────────────────────────────
    llvm::Function* func = ctx.lookupFunction(decl);
    if (!func) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "function '", ctx.pool.lookup(decl->name),
                                "' has no prototype");
        return;
    }

    // ─── 5. Skip if function already has a body ─────────────────────────
    if (!func->empty()) return;

    // ─── 6. Set up the entry block ──────────────────────────────────────
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(
        ctx.llvmCtx, "entry", func
    );
    ctx.builder.SetInsertPoint(entry);

    // ─── 7. Save the previous function context ──────────────────────────
    llvm::Function* prevFunc = ctx.currentFunction;
    llvm::Value* prevEnv = ctx.currentEnvPtr;
    TypeAST* prevReturnType = ctx.currentDeclaredReturnType;
    ctx.currentFunction = func;
    ctx.currentDeclaredReturnType = decl->funcType ? decl->funcType->returnType : nullptr;

    // ─── 8. Push a live scope for the function body ─────────────────────
    ctx.pushLiveScope();

    // ─── 9. Allocate and store parameters ──────────────────────────────
    size_t argIdx = 0;
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

                // A cls-typed parameter owns the value it received from the
                // caller. The caller retained on argument pass, so the callee
                // must release at function exit.
                if (ownsResource(param)) {
                    ctx.markAlive(param);
                }
            }
        }
        paramTypeIter = paramTypeIter->getNext();
    }

    // ─── 10. Lower the body statements ──────────────────────────────────
    // The body lives on the anon, not on the declaration.
    if (anon->body) {
        if (anon->body->isa<BlockStmtAST>()) {
            lowerBlockStmt(anon->body->as<BlockStmtAST>(), ctx);
        } else if (anon->body->isa<ReturnStmtAST>()) {
            lowerReturnStmt(anon->body->as<ReturnStmtAST>(), ctx);
        }
    }

    // ─── 11. Ensure a terminator exists (void functions) ────────────────
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

    // ─── 12. Pop the function scope (emits cleanup) ─────────────────────
    ctx.popLiveScope();

    // ─── 13. Restore the previous function context ──────────────────────
    ctx.currentFunction = prevFunc;
    ctx.currentEnvPtr = prevEnv;
    ctx.currentDeclaredReturnType = prevReturnType;

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
    // decl->llvmGlobal = global;
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
    // Only variables that actually own a heap resource are tracked.
    // classifyResource is the single source of truth for "does this
    // binding own a resource?"; ownsResource is its boolean form.
    //
    // A VarDeclAST never has FuncTypeAST (parser's looksLikeFuncDecl
    // guarantees it), so the function-typed branch of classifyResource
    // never fires here — but ownsResource handles it anyway, which means
    // we don't need a separate VarDeclAST-only classifier.
    if (ownsResource(decl)) {
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

        // ─── fn → cls coercion ─────────────────────────────────────────────
        // A `fn`-typed initializer assigned to a `cls`-typed slot is the
        // implicit widening: wrap the bare pointer in a null-env fat pointer.
        // This must run before the bfitcast below, because it changes the
        // value's shape, not just its pointer type.
        initValue = maybeCoerceFnToCls(
            initValue,
            decl->init->resolvedType,
            decl->type,
            ctx);
        if (!initValue) return;

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

    // ─── Skip generic templates ────────────────────────────────────────
    // A generic template is a family, not a concrete declaration. Sema
    // produced a specialization for every concrete type the program
    // actually uses, and those specializations appear in
    // `module->specializations`. A template has no LLVM object of its
    // own — it's the recipe, not the result.
    if (decl->isa<StructDeclAST>() && decl->as<StructDeclAST>()->isGeneric()) {
        return;
    }
    if (decl->isa<FuncDeclAST>() && decl->as<FuncDeclAST>()->isGeneric()) {
        return;
    }

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