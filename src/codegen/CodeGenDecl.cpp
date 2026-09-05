/// @file CodeGenDecl.cpp
/// @brief Implementation of declaration lowering to LLVM IR.
///
/// This file handles lowering of all declarations (functions, variables,
/// structs, enums) to LLVM IR. It operates in two phases to support
/// forward references.
///
/// ─── Two-Phase Design ──────────────────────────────────────────────────────
/// Lucid supports forward references - a function can be called before it is
/// defined. To enable this, declarations are lowered in two passes:
///
///   Phase 1 (lowerModuleDeclarations): Create all function prototypes,
///   struct types, and global variables. No function bodies are generated.
///
///   Phase 2 (lowerModuleBodies): Generate the actual code for function
///   bodies. By now, all symbols are declared and can be resolved.
///
/// ─── Generic Handling ─────────────────────────────────────────────────────
/// Generic functions and structs use a hybrid strategy:
///
///   1. DEFAULT: Type Erasure (Tagged Slots)
///      - One LLVM function/struct per generic declaration
///      - All values passed as tagged slots { tag, value }
///      - Runtime tag checking for type safety
///      - Generated immediately in Phase 1
///
///   2. OPT-IN: Monomorphization (@[specialize])
///      - Separate LLVM function/struct per concrete type instantiation
///      - No runtime overhead, direct calls
///      - Generated lazily in Phase 2 when first used
///      - Cached in GenericRegistry to avoid duplication

#include "CodeGen.hpp"
#include "generic/CodeGenGeneric.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "support/CodeGenAlloca.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>

namespace codegen {

// =============================================================================
// Declaration Lowering
// =============================================================================

/// @brief Lower any declaration to LLVM IR.
///
/// This is the main dispatch for declaration lowering. It routes to the
/// appropriate specialized function based on the declaration kind.
///
/// @note This is called from lowerModuleDeclarations() during Phase 1.
///       All declarations are processed before any function bodies.
void lowerDeclaration(DeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    switch (decl->kind) {
        case ASTKind::ImportDecl:
            // Imports are handled by the module resolver, not CodeGen.
            break;
        case ASTKind::FuncDecl:
            lowerFunctionDecl(decl->as<FuncDeclAST>(), ctx);
            break;
        case ASTKind::StructDecl:
            lowerStructDecl(decl->as<StructDeclAST>(), ctx);
            break;
        case ASTKind::EnumDecl:
            lowerEnumDecl(decl->as<EnumDeclAST>(), ctx);
            break;
        case ASTKind::VarDecl:
            lowerVarDecl(decl->as<VarDeclAST>(), ctx);
            break;
        default:
            // Other declaration kinds don't need special handling
            break;
    }
}

// =============================================================================
// Function Declaration
// =============================================================================

/// @brief Lower a function declaration (prototype only).
///
/// This creates the llvm::Function prototype for a function declaration.
/// It does NOT generate the function body - that happens in lowerFunctionBody()
/// during Phase 2.
///
/// ─── Generic Function Handling ────────────────────────────────────────────
/// Generic functions follow the hybrid strategy:
///
///   If @[specialize] is present:
///     - Register as a template for later instantiation
///     - No LLVM function created yet (generated lazily in Phase 2)
///
///   If @[specialize] is absent (default):
///     - Generate type-erased version immediately in Phase 1
///     - Uses opaque pointers and tagged slots
///     - One function works for all type instantiations
///
/// ─── Foreign Functions ────────────────────────────────────────────────────
/// Functions with @[foreign] attribute are declared as ExternalLinkage with
/// no body. The JIT or linker resolves them at runtime/link time.
///
/// ─── Closures ─────────────────────────────────────────────────────────────
/// If a function is a closure (has captured variables), it gets an extra
/// "env" parameter as its first argument. This is set in decl->hasClosure.
void lowerFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    if (ctx.lookupFunction(decl)) {
        return;
    }

    if (decl->isForeignFunction) {
        llvm::FunctionType* funcType = getFunctionType(ctx, decl->funcType, decl->hasClosure);
        // Foreign functions use the raw name, not mangled
        std::string funcName = ctx.pool.lookup(decl->name);
        llvm::Function* func = llvm::Function::Create(
            funcType,
            llvm::Function::ExternalLinkage,
            funcName,
            ctx.module
        );
        ctx.storeFunction(decl, func);
        return;
    }

    // ─── Use the mangled name from Sema ──────────────────────────────────
    if (!decl->mangledName.isValid()) {
        ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, decl->loc,
                                "INTERNAL ERROR: function '", ctx.pool.lookup(decl->name),
                                "' has no mangled name");
        return;
    }
    std::string funcName = ctx.pool.lookup(decl->mangledName);

    llvm::FunctionType* funcType = getFunctionType(ctx, decl->funcType, decl->hasClosure);
    llvm::Function* func = llvm::Function::Create(
        funcType,
        llvm::Function::ExternalLinkage,
        funcName,
        ctx.module
    );

    // ─── Set parameter names ──────────────────────────────────────────────
    size_t paramIndex = 0;

    // For closures, the first parameter is the environment pointer
    if (decl->hasClosure) {
        func->getArg(paramIndex++)->setName("env");
    }

    for (ParamAST* param : decl->funcType->params) {
        if (paramIndex < func->arg_size()) {
            std::string paramName = ctx.pool.lookup(param->name);
            func->getArg(paramIndex)->setName(paramName);
        }
        paramIndex++;
    }

    // ─── Store in context ─────────────────────────────────────────────────
    ctx.storeFunction(decl, func);
    decl->llvmFunction = func;

    // ─── Store in module's symbol table ──────────────────────────────────
    ctx.module->getOrInsertFunction(funcName, funcType);

    // ─── Track mutable functions that hold closures ──────────────────────────
    // For `let` functions that hold closures, we need to track them so their
    // environment is released when they go out of scope or are reassigned.
    //
    // const functions are immutable and never need tracking.
    // let functions without closures don't need tracking (no environment).
    if (decl->keyword == DeclKeyword::Let && decl->hasClosure) {
        ctx.markAlive(decl);
        
        // ─── Create an alloca to store the closure value ──────────────────
        // The closure value is { func_ptr, env_ptr } which is the same
        // shape as ctx.getClosureType().
        llvm::Type* closureType = ctx.getClosureType();
        llvm::AllocaInst* alloca = createAlloca(
            ctx.pool.lookup(decl->name) + "_closure",
            closureType,
            ctx
        );
        
        // ─── Build the initial closure value ──────────────────────────────
        // For a `let` function declared with a body that captures variables,
        // the initial value is { func_ptr, env_ptr } where env_ptr is the
        // environment allocated when the closure is created.
        //
        // The environment is allocated in lowerClosure when the function
        // is actually called/used. At declaration time, we store a null
        // environment initially.
        llvm::Value* closureVal = llvm::UndefValue::get(closureType);
        
        // Function pointer
        llvm::Value* funcPtr = ctx.builder.CreatePointerCast(
            func,
            llvm::PointerType::get(ctx.llvmCtx, 0),
            "func_ptr"
        );
        closureVal = ctx.builder.CreateInsertValue(closureVal, funcPtr, 0);
        
        // Environment pointer (initially null - allocated when closure is created)
        closureVal = ctx.builder.CreateInsertValue(
            closureVal,
            llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx.llvmCtx, 0)),
            1
        );
        
        ctx.builder.CreateStore(closureVal, alloca);
        ctx.storeValue(decl, alloca);
        
        Trace::detail("Tracked mutable closure function: ", ctx.pool.lookup(decl->name));
    }

    Trace::detail("Lowered function declaration: ", funcName,
                " (", func->arg_size(), " params, ",
                decl->hasClosure ? "closure" : "plain", ")");
}

/// @brief Lower a function body (second pass).
///
/// This generates the actual LLVM IR for a function's body. It is called
/// during Phase 2, after all declarations have been lowered.
///
/// ─── Generic Function Body Lowering ──────────────────────────────────────
/// For @[specialize] generic functions, the body is generated for each
/// instantiation separately. The body is cloned with type substitutions.
///
/// For type-erased generic functions, the body is generated once and works
/// for all instantiations using tagged slots.
void lowerFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    // ─── Skip foreign functions ──────────────────────────────────────────
    if (decl->isForeignFunction) {
        return;
    }

    // ─── Generic functions with @[specialize] ────────────────────────────
    // The body is generated lazily when instantiated.
    // We don't generate a body here because there's no single body
    // for all instantiations - each instantiation gets its own copy
    // with types substituted.
    if (isGenericFunction(decl) && shouldSpecialize(decl)) {
        // Body will be generated when getOrCreateSpecializedFunction() is called
        // and then lowerSpecializedFunctionBody() is invoked.
        Trace::detail("Generic specialized function '",
                          ctx.pool.lookup(decl->name),
                          "' body will be generated lazily");
        return;
    }

    // ─── Get the function ─────────────────────────────────────────────────
    llvm::Function* func = ctx.lookupFunction(decl);
    if (!func) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, decl->loc,
                                "function '", ctx.pool.lookup(decl->name),
                                "' not found in symbol table");
        return;
    }

    // ─── Skip if already has a body ──────────────────────────────────────
    if (!func->empty()) {
        return;
    }

    // ─── Lower the function body ─────────────────────────────────────────
    lowerFunctionBodyInternal(decl, func, ctx);
}

/// @brief Internal function to lower a function body.
///
/// This is called for non-generic functions and for type-erased generic
/// functions. It creates the entry block, lowers parameters, and generates
/// the function body.
void lowerFunctionBodyInternal(FuncDeclAST* decl, llvm::Function* func, CodeGenContext& ctx) {
    // ─── Push function context ────────────────────────────────────────────
    ctx.setCurrentFunction(func);

    // ─── Create entry block ──────────────────────────────────────────────
    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "entry",
        func
    );
    ctx.builder.SetInsertPoint(entryBlock);

    // ─── Push scope for parameters ──────────────────────────────────────
    size_t argIndex = 0;

    // For closures, the first parameter is the environment pointer
    if (decl->hasClosure) {
        llvm::Value* envPtr = func->getArg(argIndex++);
        // Store the environment pointer for later use
        // We'll need this for closure calls
        ctx.storeValue(nullptr, envPtr);
    }

    // ─── Lower parameters (create allocas and store arguments) ──────────
    // Adjacent parameter groups are represented by nested functions, so this
    // function owns only the parameters in its outermost group.
    for (ParamAST* param : decl->funcType->params) {
        lowerParam(param, ctx);
        argIndex++;
    }

    // ─── Lower the body ───────────────────────────────────────────────────
    if (decl->body) {
        lowerStatement(decl->body, ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, decl->loc,
                                "function '", ctx.pool.lookup(decl->name),
                                "' has no body");
    }

    // ─── Pop scope and function context ──────────────────────────────────
    ctx.setCurrentFunction(nullptr);

    // ─── Verify the function ─────────────────────────────────────────────
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyFunction(*func, &errorStream)) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, decl->loc,
                                "function '", ctx.pool.lookup(decl->name),
                                "' failed verification: ", error);
    }

    Trace::detail("Lowered function body: ", ctx.pool.lookup(decl->name));
}

/// @brief Lower a specialized function body for a specific instantiation.
///
/// This is called when @[specialize] is used and a concrete instantiation
/// is needed. It clones the generic function body with type substitutions.
void lowerSpecializedFunctionBody(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    llvm::Function* specializedFunc,
    CodeGenContext& ctx
) {
    if (!funcDecl || !specializedFunc) return;

    // ─── Save and clear the value map for this instantiation ──────────────
    // This prevents collisions between different instantiations of the same
    // generic function. Each instantiation gets its own isolated value map.
    //
    // IMPORTANT: This also handles the closure environment pointer issue,
    // since closures are lowered as part of the function body.
    auto savedValues = std::move(ctx.values);
    ctx.values.clear();

    // ─── Push function context ────────────────────────────────────────────
    ctx.setCurrentFunction(specializedFunc);

    // ─── Create entry block ──────────────────────────────────────────────
    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "entry",
        specializedFunc
    );
    ctx.builder.SetInsertPoint(entryBlock);

    // ─── Lower parameters with substituted types ─────────────────────────
    size_t argIndex = 0;

    // For closures, the first parameter is the environment pointer
    if (funcDecl->hasClosure) {
        llvm::Value* envPtr = specializedFunc->getArg(argIndex++);
        // Store environment pointer in a separate field, not in ctx.values
        // to avoid collisions. Use a dedicated field on CodeGenContext.
        ctx.currentEnvPtr = envPtr;
    }

    // Lower the outermost parameter group with substituted types. Any inner
    // group is lowered when its nested function expression is emitted.
    for (ParamAST* param : funcDecl->funcType->params) {
        GenericSubstitution subst{funcDecl->genericParams, typeArgs};
        llvm::Type* llvmType = getType(ctx, param->type, &subst);
        if (!llvmType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
                                    "parameter '", ctx.pool.lookup(param->name),
                                    "' has invalid type in specialization");
            return;
        }

        std::string paramName = ctx.pool.lookup(param->name);
        llvm::AllocaInst* alloca = createAlloca(paramName, llvmType, ctx);

        // Store the argument into the alloca
        llvm::Value* argValue = specializedFunc->getArg(argIndex);
        ctx.builder.CreateStore(argValue, alloca);

        // Store in symbol table (now isolated per instantiation)
        ctx.storeValue(param, alloca);
        param->llvmAlloca = alloca;
        param->llvmValue = argValue;

        argIndex++;
    }

    // ─── Lower the body ───────────────────────────────────────────────────
    if (funcDecl->body) {
        lowerStatement(funcDecl->body, ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, funcDecl->loc,
                                "specialized function '", 
                                specializedFunc->getName().str(),
                                "' has no body");
    }

    // ─── Pop scope and function context ──────────────────────────────────
    ctx.setCurrentFunction(nullptr);
    ctx.currentEnvPtr = nullptr;

    // ─── Restore the saved value map ──────────────────────────────────────
    ctx.values = std::move(savedValues);

    // ─── Verify the function ─────────────────────────────────────────────
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyFunction(*specializedFunc, &errorStream)) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, funcDecl->loc,
                                "specialized function '", 
                                specializedFunc->getName().str(),
                                "' failed verification: ", error);
    }

    Trace::detail("Lowered specialized function body: ", 
                specializedFunc->getName().str());
}

/// @brief Lower a function parameter.
///
/// Each parameter gets an alloca in the function's entry block. The argument
/// value is stored into this alloca. This is necessary because:
///   1. Parameters can be referenced as l-values (e.g., &param)
///   2. Parameters can be mutable (if declared with `let`)
///   3. It provides a consistent way to access parameters
///
/// IMPORTANT: Parameters do NOT own their environment. The caller owns the
/// closure environment, not the callee. So we should NOT mark parameters as alive.
void lowerParam(ParamAST* param, CodeGenContext& ctx) {
    if (!param) return;

    // ─── Get LLVM type ────────────────────────────────────────────────────
    llvm::Type* paramType = nullptr;
    if (param->isVariadic) {
        paramType = ctx.getSliceType();
    } else if (param->type && param->type->isa<FuncTypeAST>()) {
        paramType = getFunctionRuntimeType(
            ctx,
            param->type->as<FuncTypeAST>(),
            true
        );
    } else {
        paramType = getType(ctx, param->type);
    }
    if (!paramType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
                                "parameter '", ctx.pool.lookup(param->name),
                                "' has invalid type");
        return;
    }

    // ─── Get the LLVM function and argument ──────────────────────────────
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
                                "parameter '", ctx.pool.lookup(param->name),
                                "' has no current function");
        return;
    }

    // ─── Look up the argument value ──────────────────────────────────────
    llvm::Value* argValue = nullptr;
    for (auto& arg : func->args()) {
        if (arg.getName() == ctx.pool.lookup(param->name)) {
            argValue = &arg;
            break;
        }
    }

    if (!argValue) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
                                "parameter '", ctx.pool.lookup(param->name),
                                "' not found in function arguments");
        return;
    }

    // ─── Create alloca for the parameter ─────────────────────────────────
    llvm::AllocaInst* alloca = createAlloca(
        ctx.pool.lookup(param->name),
        paramType,
        ctx
    );

    // ─── Store the argument value into the alloca ────────────────────────
    ctx.builder.CreateStore(argValue, alloca);

    // ─── Store in symbol table ────────────────────────────────────────────
    ctx.storeValue(param, alloca);
    param->llvmAlloca = alloca;
    param->llvmValue = argValue;
}

// =============================================================================
// Variable Declaration
// =============================================================================

/// @brief Lower a variable declaration.
///
/// Variables can be either module-level (globals) or local (allocas).
///
/// IMPORTANT: Only variables that own heap memory AND are mutable need tracking.
void lowerVarDecl(VarDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    llvm::Type* varType = getType(ctx, decl->type);
    if (!varType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, decl->loc,
                                "variable '", ctx.pool.lookup(decl->name),
                                "' has invalid type");
        return;
    }

    bool isModuleLevel = ctx.module && ctx.getCurrentFunction() == nullptr;

    if (isModuleLevel) {
        // ─── Use the mangled name from Sema ──────────────────────────────
        std::string varName;
        if (decl->mangledName.isValid()) {
            varName = ctx.pool.lookup(decl->mangledName);
        } else {
            ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, decl->loc,
                                      "variable '", ctx.pool.lookup(decl->name),
                                      "' has no mangled name - using raw name");
            varName = ctx.pool.lookup(decl->name);
        }

        // ─── Create global with the mangled name ──────────────────────────
        llvm::GlobalVariable* global = new llvm::GlobalVariable(
            *ctx.module,
            varType,
            decl->isConst(),
            llvm::GlobalValue::ExternalLinkage,
            llvm::Constant::getNullValue(varType),
            varName
        );

        ctx.storeValue(decl, global);
        decl->llvmGlobal = global;

        // ─── Handle initialization ──────────────────────────────────────
        if (decl->init) {
            if (decl->init->isConst) {
                llvm::Value* initValue = lowerExpression(decl->init, ctx);
                if (initValue) {
                    if (llvm::Constant* constInit = llvm::dyn_cast<llvm::Constant>(initValue)) {
                        global->setInitializer(constInit);
                    }
                }
            } else {
                // ─── Non-constant - defer to __init_globals ────────────
                ctx.pendingGlobals.push_back({
                    decl,
                    decl->init,
                    global,
                    ctx.currentModule,
                    decl->orderInModule  // Only need order within module
                });
            }
        }

        return;
    }

    // ─── Local variable ───────────────────────────────────────────────────
    std::string varName = ctx.pool.lookup(decl->name);
    llvm::AllocaInst* alloca = createAlloca(varName, varType, ctx);

    if (decl->init) {
        llvm::Value* initValue = lowerExpression(decl->init, ctx);
        if (initValue) {
            ctx.builder.CreateStore(initValue, alloca);
        }
    } else {
        ctx.builder.CreateStore(
            llvm::Constant::getNullValue(varType),
            alloca
        );
    }

    ctx.storeValue(decl, alloca);
    decl->llvmAlloca = alloca;
    
    // ─── Mark alive if the variable owns heap memory ──────────────────────
    // VarDeclAST can NEVER store function types! Only:
    //   - Dynamic arrays ([*]T) - own heap memory
    //   - Strings (string) - own heap memory
    //   - Structs containing these - handled by struct cleanup
    //
    // NOTE: No FuncTypeAST check here because VarDeclAST can't store functions!
    if (!isModuleLevel) {
        TypeAST* type = decl->type;
        if (type) {
            bool needsCleanup = false;
            
            if (type->isa<ArrayTypeAST>()) {
                ArrayTypeAST* array = type->as<ArrayTypeAST>();
                needsCleanup = array->isDynamic();
            } else if (type->isa<PrimitiveTypeAST>()) {
                PrimitiveTypeAST* prim = type->as<PrimitiveTypeAST>();
                needsCleanup = (prim->primitiveKind == PrimitiveKind::String);
            }
            
            if (needsCleanup) {
                ctx.markAlive(decl);
            }
        }
    }
}

// =============================================================================
// Struct Declaration
// =============================================================================

/// @brief Lower a struct declaration to an LLVM struct type.
///
/// ─── Generic Struct Handling ─────────────────────────────────────────────
/// Generic structs follow the hybrid strategy:
///
///   If @[specialize] is present:
///     - Register as a template for later instantiation
///     - No LLVM struct type created yet (generated lazily in Phase 2)
///     - Each instantiation gets its own concrete struct type
///
///   If @[specialize] is absent (default):
///     - Generate type-erased version immediately in Phase 1
///     - One struct type with tagged slots for all fields
///     - Works for all type instantiations with runtime tag checks
///
/// ─── Self-Reference Handling ─────────────────────────────────────────────
/// Structs that reference themselves (e.g., linked lists) require special
/// handling to avoid infinite recursion. The key insight is that the
/// self-referential field must be stored as a pointer.
///
/// The lowering process for self-referential structs:
///   1. Create an opaque (incomplete) struct type as a forward declaration
///   2. Store the opaque type in the cache
///   3. Build field types - when getType() sees the self-reference,
///      it returns a pointer to the opaque type
///   4. The pointer breaks the recursion
///   5. Call setBody() to define the struct with all field types
void lowerStructDecl(StructDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    // ─── Skip if already lowered ──────────────────────────────────────────
    if (ctx.lookupStruct(decl)) {
        return;
    }

    // ─── Generic struct handling ──────────────────────────────────────────
    if (isGenericStruct(decl)) {
        if (shouldSpecialize(decl)) {
            // ─── @[specialize]: Register as template for lazy generation ──
            // The actual specialized struct types are generated on-demand
            // when getOrCreateSpecializedStruct() is called in Phase 2.
            Trace::detail("Registered generic struct template: ",
                       ctx.pool.lookup(decl->name));
            return;
        } else {
            // ─── Default: Generate type-erased version ────────────────────
            // One struct type with tagged slots for all fields.
            // Works for all type instantiations with runtime tag checks.
            llvm::Type* erasedType = generateErasedGenericStruct(decl, ctx);
            if (erasedType) {
                ctx.cacheStruct(decl, llvm::cast<llvm::StructType>(erasedType));
                decl->llvmType = llvm::cast<llvm::StructType>(erasedType);
                Trace::detail("Lowered type-erased generic struct: ",
                           ctx.pool.lookup(decl->name));
            }
            return;
        }
    }

    // ─── Non-generic struct - normal lowering ────────────────────────────
    // ─── Use the mangled name from Sema ──────────────────────────────────────
    // Sema generates mangled names for all structs to avoid name collisions.
    if (!decl->mangledName.isValid()) {
        llvm_unreachable("Struct has no mangled name - Sema bug");
    }
    std::string structName = ctx.pool.lookup(decl->mangledName);

    // ─── Check if struct type already exists ──────────────────────────────
    llvm::StructType* structType = llvm::StructType::getTypeByName(
        ctx.llvmCtx,
        structName
    );

    // ─── Create opaque type FIRST (for self-referential structs) ─────────
    // This is the key to handling self-referential structs.
    if (!structType) {
        structType = llvm::StructType::create(ctx.llvmCtx, structName);
    }
    
    // ─── Cache the opaque type BEFORE building fields ────────────────────
    ctx.cacheStruct(decl, structType);
    
    // ─── Build field types ─────────────────────────────────────────────────
    std::vector<llvm::Type*> fieldTypes;

    for (FieldDeclAST* field : decl->fields) {
        llvm::Type* fieldType = nullptr;
        if (field->type && field->type->isa<FuncTypeAST>()) {
            fieldType = getFunctionRuntimeType(
                ctx,
                field->type->as<FuncTypeAST>(),
                true
            );
        } else {
            fieldType = getType(ctx, field->type);
        }
        if (!fieldType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, field->loc,
                                    "field '", ctx.pool.lookup(field->name),
                                    "' has invalid type");
            fieldType = llvm::Type::getInt8Ty(ctx.llvmCtx);
        }
        fieldTypes.push_back(fieldType);
    }

    // ─── Define the opaque struct ─────────────────────────────────────────
    if (structType->isOpaque()) {
        structType->setBody(fieldTypes);
    }

    // ─── Store in cache ───────────────────────────────────────────────────
    ctx.cacheStruct(decl, structType);
    decl->llvmType = structType;

    Trace::detail("Lowered struct: ", structName, " (", fieldTypes.size(), " fields)");
}

// =============================================================================
// Enum Declaration
// =============================================================================

/// @brief Lower an enum declaration to integer constants.
///
/// Enums in Lucid are simple integer enumerations. Each variant has an
/// explicit integer value.
void lowerEnumDecl(EnumDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    // ─── Get backing type ──────────────────────────────────────────────────
    llvm::IntegerType* backingType = getEnumType(ctx, decl);
    if (!backingType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, decl->loc,
                                "enum '", ctx.pool.lookup(decl->name),
                                "' has invalid backing type");
        return;
    }

    // ─── Get the mangled enum name ────────────────────────────────────────
    std::string enumName;
    if (decl->mangledName.isValid()) {
        enumName = ctx.pool.lookup(decl->mangledName);
    } else {
        ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, decl->loc,
                                  "enum '", ctx.pool.lookup(decl->name),
                                  "' has no mangled name, using raw name");
        enumName = ctx.pool.lookup(decl->name);
    }

    // ─── Clear existing variant constants ─────────────────────────────────
    decl->variantConstants.clear();
    decl->variantConstants.reserve(decl->variants.size());

    // ─── Create constants for each variant ────────────────────────────────
    for (EnumVariantAST* variant : decl->variants) {
        llvm::ConstantInt* constVal = llvm::ConstantInt::get(
            backingType,
            variant->value,
            true
        );
        decl->variantConstants.push_back(constVal);

        variant->llvmValue = constVal;

        // ─── Use mangled name for the variant symbol ──────────────────────
        std::string varName = enumName + "." + ctx.pool.lookup(variant->name);
        new llvm::GlobalVariable(
            *ctx.module,
            backingType,
            true,
            llvm::GlobalValue::ExternalLinkage,
            constVal,
            varName
        );

        Trace::detail("Lowered enum variant: ", varName, " = ",
                           variant->value);
    }

    // ─── Store backing type ───────────────────────────────────────────────
    decl->backingLLVMType = backingType;

    Trace::detail("Lowered enum: ", enumName, " (",
                decl->variantConstants.size(), " variants)");
}

// =============================================================================
// Specialized Function Body Instantiation
// =============================================================================

/// @brief Instantiate a specialized function body for a generic function.
///
/// This is called from getOrCreateSpecializedFunction() when a new
/// instantiation is needed. It creates the function body with substituted types.
///
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments for this instantiation.
/// @param specializedFunc The specialized LLVM function to generate the body for.
/// @param ctx The code generation context.
void instantiateSpecializedFunctionBody(
    FuncDeclAST* funcDecl,
    const std::vector<TypeAST*>& typeArgs,
    llvm::Function* specializedFunc,
    CodeGenContext& ctx
) {
    if (!funcDecl || !specializedFunc) return;
    
    // ─── Check if already has a body ──────────────────────────────────────
    if (!specializedFunc->empty()) {
        return;
    }
    
    // ─── Lower the specialized function body ─────────────────────────────
    lowerSpecializedFunctionBody(funcDecl, typeArgs, specializedFunc, ctx);
}

} // namespace codegen