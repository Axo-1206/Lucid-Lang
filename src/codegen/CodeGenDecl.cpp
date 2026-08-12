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
#include "CodeGenType.hpp"
#include "CodeGenGeneric.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"
#include "support/CodeGenHelpers.hpp"

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
/// @param decl The declaration to lower.
/// @param ctx The code generation context.
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
///
/// @param decl The function declaration to lower.
/// @param ctx The code generation context.
void lowerFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    // ─── Skip if already lowered ──────────────────────────────────────────
    if (ctx.lookupFunction(decl)) {
        return;
    }

    // ─── Check if this is a foreign function ──────────────────────────────
    if (decl->isForeignFunction) {
        // Foreign functions are declared but not defined.
        // They will be resolved by the JIT or linker.
        llvm::FunctionType* funcType = getFunctionType(ctx, decl->funcType, decl->hasClosure);
        if (!funcType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, decl->loc,
                                    "function '", ctx.pool.lookup(decl->name),
                                    "' has invalid function type");
            return;
        }

        std::string funcName = ctx.pool.lookup(decl->name);
        llvm::Function* func = llvm::Function::Create(
            funcType,
            llvm::Function::ExternalLinkage,
            funcName,
            ctx.module
        );

        ctx.storeFunction(decl, func);
        decl->llvmFunction = func;

        LOG_CODEGEN("Lowered foreign function declaration: ", funcName);
        return;
    }

    // ─── Generic function handling ────────────────────────────────────────
    if (isGenericFunction(decl)) {
        if (shouldSpecialize(decl)) {
            // ─── @[specialize]: Register as template for lazy generation ──
            // The actual specialized functions are generated on-demand
            // when getOrCreateSpecializedFunction() is called in Phase 2.
            LOG_CODEGEN("Registered generic function template: ",
                       ctx.pool.lookup(decl->name));
            return;
        } else {
            // ─── Default: Generate type-erased version ────────────────────
            // One function with opaque pointers and tagged slots.
            // Works for all type instantiations with runtime tag checks.
            llvm::Function* erasedFunc = generateErasedGenericFunction(decl, ctx);
            if (erasedFunc) {
                ctx.storeFunction(decl, erasedFunc);
                decl->llvmFunction = erasedFunc;
                LOG_CODEGEN("Lowered type-erased generic function: ",
                           ctx.pool.lookup(decl->name));
            }
            return;
        }
    }

    // ─── Non-generic function - normal lowering ───────────────────────────
    llvm::FunctionType* funcType = getFunctionType(ctx, decl->funcType, decl->hasClosure);
    if (!funcType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, decl->loc,
                                "function '", ctx.pool.lookup(decl->name),
                                "' has invalid function type");
        return;
    }

    std::string funcName = ctx.pool.lookup(decl->name);
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

    // For each parameter group in the function type
    const FuncTypeAST* currentType = decl->funcType;
    while (currentType) {
        for (ParamAST* param : currentType->params) {
            if (paramIndex < func->arg_size()) {
                std::string paramName = ctx.pool.lookup(param->name);
                func->getArg(paramIndex)->setName(paramName);
            }
            paramIndex++;
        }
        currentType = currentType->getNext();
    }

    // ─── Store in context ─────────────────────────────────────────────────
    ctx.storeFunction(decl, func);
    decl->llvmFunction = func;

    // ─── Store in module's symbol table ──────────────────────────────────
    ctx.module->getOrInsertFunction(
        funcName,
        funcType
    );

    LOG_CODEGEN("Lowered function declaration: ", funcName,
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
///
/// @param decl The function declaration whose body to lower.
/// @param ctx The code generation context.
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
        LOG_CODEGEN_DETAIL("Generic specialized function '",
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
///
/// @param decl The function declaration.
/// @param func The LLVM function to generate the body for.
/// @param ctx The code generation context.
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
    const FuncTypeAST* currentType = decl->funcType;
    while (currentType) {
        for (ParamAST* param : currentType->params) {
            lowerParam(param, ctx);
            argIndex++;
        }
        currentType = currentType->getNext();
    }

    // ─── Lower the body ───────────────────────────────────────────────────
    if (decl->body) {
        lowerStatement(const_cast<StmtAST*>(decl->body), ctx);
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

    LOG_CODEGEN("Lowered function body: ", ctx.pool.lookup(decl->name));
}

/// @brief Lower a specialized function body for a specific instantiation.
///
/// This is called when @[specialize] is used and a concrete instantiation
/// is needed. It clones the generic function body with type substitutions.
///
/// @param funcDecl The generic function declaration.
/// @param typeArgs The concrete type arguments for this instantiation.
/// @param specializedFunc The specialized LLVM function to generate the body for.
/// @param ctx The code generation context.
void lowerSpecializedFunctionBody(
    const FuncDeclAST* funcDecl,
    const std::vector<const TypeAST*>& typeArgs,
    llvm::Function* specializedFunc,
    CodeGenContext& ctx
) {
    if (!funcDecl || !specializedFunc) return;

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
    // Parameters are the same as the generic function but with types substituted
    size_t argIndex = 0;

    // For closures, the first parameter is the environment pointer
    if (funcDecl->hasClosure) {
        llvm::Value* envPtr = specializedFunc->getArg(argIndex++);
        ctx.storeValue(nullptr, envPtr);
    }

    // Lower each parameter with the substituted type
    const FuncTypeAST* currentType = funcDecl->funcType;
    while (currentType) {
        for (ParamAST* param : currentType->params) {
            // Get the substituted type for this parameter
            const TypeAST* substitutedType = substituteGenericType(
                param->type,
                funcDecl->genericParams,
                typeArgs,
                ctx
            );
            
            // Create alloca with the substituted type
            llvm::Type* llvmType = getType(ctx, substitutedType);
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
            
            // Store in symbol table
            ctx.storeValue(param, alloca);
            param->llvmAlloca = alloca;
            param->llvmValue = argValue;
            
            argIndex++;
        }
        currentType = currentType->getNext();
    }

    // ─── Lower the body ───────────────────────────────────────────────────
    // The body AST is the same, but type references to generic parameters
    // are now resolved via the substituted types in the context.
    if (funcDecl->body) {
        lowerStatement(const_cast<StmtAST*>(funcDecl->body), ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, funcDecl->loc,
                                "specialized function '", 
                                specializedFunc->getName().str(),
                                "' has no body");
    }

    // ─── Pop scope and function context ──────────────────────────────────
    ctx.setCurrentFunction(nullptr);

    // ─── Verify the function ─────────────────────────────────────────────
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyFunction(*specializedFunc, &errorStream)) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, funcDecl->loc,
                                "specialized function '", 
                                specializedFunc->getName().str(),
                                "' failed verification: ", error);
    }

    LOG_CODEGEN("Lowered specialized function body: ", 
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
/// @param param The parameter to lower.
/// @param ctx The code generation context.
void lowerParam(ParamAST* param, CodeGenContext& ctx) {
    if (!param) return;

    // ─── Get LLVM type ────────────────────────────────────────────────────
    llvm::Type* paramType = getType(ctx, param->type);
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

    LOG_CODEGEN_DETAIL("Lowered parameter: ", ctx.pool.lookup(param->name));
}

// =============================================================================
// Variable Declaration
// =============================================================================

/// @brief Lower a variable declaration.
///
/// Variables can be either module-level (globals) or local (allocas).
///
/// @param decl The variable declaration to lower.
/// @param ctx The code generation context.
void lowerVarDecl(VarDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    // ─── Get LLVM type ────────────────────────────────────────────────────
    llvm::Type* varType = getType(ctx, decl->type);
    if (!varType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, decl->loc,
                                "variable '", ctx.pool.lookup(decl->name),
                                "' has invalid type");
        return;
    }

    // ─── Check if this is a module-level variable ────────────────────────
    bool isModuleLevel = ctx.module && ctx.getCurrentFunction() == nullptr;

    if (isModuleLevel) {
        // ─── Module-level global variable ──────────────────────────────────
        std::string varName = ctx.pool.lookup(decl->name);

        bool isConst = decl->isConst();

        llvm::GlobalVariable* global = new llvm::GlobalVariable(
            *ctx.module,
            varType,
            isConst,
            llvm::GlobalValue::ExternalLinkage,
            nullptr,
            varName
        );

        // ─── Set initializer if present ──────────────────────────────────
        if (decl->init) {
            llvm::Value* initValue = lowerExpression(decl->init, ctx);
            if (initValue) {
                if (llvm::Constant* constInit = llvm::dyn_cast<llvm::Constant>(initValue)) {
                    global->setInitializer(constInit);
                } else {
                    ctx.diagnostics.errorAt(DiagCode::Sem_InvalidAssignment, decl->init->loc,
                                            "global variable '", varName,
                                            "' has non-constant initializer");
                }
            }
        } else {
            global->setInitializer(llvm::Constant::getNullValue(varType));
        }

        ctx.storeValue(decl, global);
        decl->llvmGlobal = global;

        LOG_CODEGEN("Lowered global variable: ", varName);
        return;
    }

    // ─── Local variable ───────────────────────────────────────────────────
    std::string varName = ctx.pool.lookup(decl->name);
    llvm::AllocaInst* alloca = createAlloca(varName, varType, ctx);

    // ─── Lower initializer if present ────────────────────────────────────
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

    LOG_CODEGEN_DETAIL("Lowered local variable: ", varName);
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
///
/// @param decl The struct declaration to lower.
/// @param ctx The code generation context.
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
            LOG_CODEGEN("Registered generic struct template: ",
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
                LOG_CODEGEN("Lowered type-erased generic struct: ",
                           ctx.pool.lookup(decl->name));
            }
            return;
        }
    }

    // ─── Non-generic struct - normal lowering ────────────────────────────
    std::string structName = ctx.pool.lookup(decl->name);

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

    for (const FieldDeclAST* field : decl->fields) {
        llvm::Type* fieldType = getType(ctx, field->type);
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

    LOG_CODEGEN("Lowered struct: ", structName, " (", fieldTypes.size(), " fields)");
}

// =============================================================================
// Enum Declaration
// =============================================================================

/// @brief Lower an enum declaration to integer constants.
///
/// Enums in Lucid are simple integer enumerations. Each variant has an
/// explicit integer value.
///
/// @param decl The enum declaration to lower.
/// @param ctx The code generation context.
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

    // ─── Create constants for each variant ────────────────────────────────
    std::vector<llvm::ConstantInt*> variantConstants;
    variantConstants.reserve(decl->variants.size());

    for (const EnumVariantAST* variant : decl->variants) {
        llvm::ConstantInt* constVal = llvm::ConstantInt::get(
            backingType,
            variant->value,
            true
        );
        variantConstants.push_back(constVal);

        const_cast<EnumVariantAST*>(variant)->llvmValue = constVal;

        std::string varName = ctx.pool.lookup(decl->name) + "." +
                             ctx.pool.lookup(variant->name);
        new llvm::GlobalVariable(
            *ctx.module,
            backingType,
            true,
            llvm::GlobalValue::ExternalLinkage,
            constVal,
            varName
        );

        LOG_CODEGEN_DETAIL("Lowered enum variant: ", varName, " = ",
                           variant->value);
    }

    decl->backingLLVMType = backingType;

    LOG_CODEGEN("Lowered enum: ", ctx.pool.lookup(decl->name), " (",
                variantConstants.size(), " variants)");
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
    const FuncDeclAST* funcDecl,
    const std::vector<const TypeAST*>& typeArgs,
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