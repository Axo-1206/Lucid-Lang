/// @file CodeGenDecl.cpp
/// @brief Implementation of declaration lowering to LLVM IR.

#include "CodeGen.hpp"
#include "CodeGenType.hpp"
#include "debug/DebugUtils.hpp"
#include "core/ast/DeclAST.hpp"
#include "core/ast/StmtAST.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"

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

    // ─── Get function type ─────────────────────────────────────────────────
    llvm::FunctionType* funcType = getFunctionType(ctx, decl->funcType, decl->hasClosure);
    if (!funcType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, decl->loc,
                                "function '", ctx.pool.lookup(decl->name),
                                "' has invalid function type");
        return;
    }

    // ─── Create function ───────────────────────────────────────────────────
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
    // This allows other functions to call this one by name
    ctx.module->getOrInsertFunction(
        funcName,
        funcType
    );

    LOG_CODEGEN("Lowered function declaration: ", funcName,
                " (", func->arg_size(), " params, ",
                decl->hasClosure ? "closure" : "plain", ")");
}

void lowerFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    // ─── Skip foreign functions ──────────────────────────────────────────
    if (decl->isForeignFunction) {
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
    // We store parameters in the context's value map
    size_t argIndex = 0;

    // For closures, the first parameter is the environment pointer
    if (decl->hasClosure) {
        llvm::Value* envPtr = func->getArg(argIndex++);
        // Store the environment pointer for later use
        // We'll need this for closure calls
        ctx.storeValue(nullptr, envPtr); // We'll use a special key or field
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
    // Find the argument by name
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
    // This allows taking the address of parameters (for & or mutability)
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

        // Check if this is a const (read-only) or let (mutable)
        bool isConst = decl->isConst();

        llvm::GlobalVariable* global = new llvm::GlobalVariable(
            *ctx.module,
            varType,
            isConst,
            llvm::GlobalValue::ExternalLinkage,
            nullptr, // Initializer set below
            varName
        );

        // ─── Set initializer if present ──────────────────────────────────
        if (decl->init) {
            // Lower the initializer expression
            llvm::Value* initValue = lowerExpression(decl->init, ctx);
            if (initValue) {
                if (llvm::Constant* constInit = llvm::dyn_cast<llvm::Constant>(initValue)) {
                    global->setInitializer(constInit);
                } else {
                    // Non-constant initializer for global - need to handle
                    // This should have been caught by Sema
                    ctx.diagnostics.errorAt(DiagCode::Sem_InvalidAssignment, decl->init->loc,
                                            "global variable '", varName,
                                            "' has non-constant initializer");
                }
            }
        } else {
            // No initializer - zero initialize
            global->setInitializer(llvm::Constant::getNullValue(varType));
        }

        // ─── Store in symbol table ──────────────────────────────────────
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
        // No initializer - zero initialize (or leave uninitialized)
        // For safety, we'll zero initialize
        ctx.builder.CreateStore(
            llvm::Constant::getNullValue(varType),
            alloca
        );
    }

    // ─── Store in symbol table ────────────────────────────────────────────
    ctx.storeValue(decl, alloca);
    decl->llvmAlloca = alloca;

    LOG_CODEGEN_DETAIL("Lowered local variable: ", varName);
}

// =============================================================================
// Struct Declaration
// =============================================================================

void lowerStructDecl(StructDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    // ─── Skip if already lowered ──────────────────────────────────────────
    if (ctx.lookupStruct(decl)) {
        return;
    }

    // ─── Get struct name ──────────────────────────────────────────────────
    std::string structName = ctx.pool.lookup(decl->name);

    // ─── Check if struct type already exists ──────────────────────────────
    llvm::StructType* structType = llvm::StructType::getTypeByName(
        ctx.llvmCtx,
        structName
    );

    if (structType) {
        // Type already exists - check if it's already defined
        if (structType->isOpaque()) {
            // It's an opaque forward declaration - we need to define it
            // We'll continue to define it below
        } else {
            // Already fully defined - skip
            ctx.cacheStruct(decl, structType);
            return;
        }
    }

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

    // ─── Create or update struct type ─────────────────────────────────────
    if (structType && structType->isOpaque()) {
        // Define the opaque type
        structType->setBody(fieldTypes);
    } else {
        // Create a new struct type
        structType = llvm::StructType::create(
            ctx.llvmCtx,
            fieldTypes,
            structName
        );
    }

    // ─── Store in cache ───────────────────────────────────────────────────
    ctx.cacheStruct(decl, structType);
    decl->llvmType = structType;

    LOG_CODEGEN("Lowered struct: ", structName, " (", fieldTypes.size(), " fields)");
}

// =============================================================================
// Enum Declaration
// =============================================================================

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
            true // Signed
        );
        variantConstants.push_back(constVal);

        // Store the constant on the variant for later use
        const_cast<EnumVariantAST*>(variant)->llvmValue = constVal;

        // Register the variant as a named constant in the module
        std::string varName = ctx.pool.lookup(decl->name) + "." +
                             ctx.pool.lookup(variant->name);
        new llvm::GlobalVariable(
            *ctx.module,
            backingType,
            true, // const
            llvm::GlobalValue::ExternalLinkage,
            constVal,
            varName
        );

        LOG_CODEGEN_DETAIL("Lowered enum variant: ", varName, " = ",
                           variant->value);
    }

    // ─── Store constants in declaration ──────────────────────────────────
    // Build the variant constants span
    // Note: This requires mutable access to the declaration
    // We'll need to use const_cast or store in a separate map
    // For now, we'll just store in the context
    // TODO: Store variant constants in the context

    LOG_CODEGEN("Lowered enum: ", ctx.pool.lookup(decl->name), " (",
                variantConstants.size(), " variants)");
}

// =============================================================================
// Helpers
// =============================================================================

llvm::AllocaInst* createAlloca(
    const std::string& name,
    llvm::Type* type,
    CodeGenContext& ctx
) {
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        return nullptr;
    }

    // ─── Get the entry block of the current function ─────────────────────
    llvm::BasicBlock* entryBlock = &func->getEntryBlock();

    // ─── Insert at the start of the entry block ───────────────────────────
    llvm::IRBuilder<> builder(ctx.llvmCtx);
    builder.SetInsertPoint(entryBlock, entryBlock->getFirstInsertionPt());

    return builder.CreateAlloca(type, nullptr, name);
}

llvm::BasicBlock* createBlock(const std::string& name, CodeGenContext& ctx) {
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        return nullptr;
    }

    return llvm::BasicBlock::Create(ctx.llvmCtx, name, func);
}

llvm::Value* loadIfNeeded(
    llvm::Value* value,
    bool isLValue,
    CodeGenContext& ctx
) {
    if (!value) return nullptr;

    // ─── If it's an l-value (pointer), load the value ─────────────────────
    if (isLValue) {
        llvm::Type* valueType = value->getType();
        if (valueType->isPointerTy()) {
            return ctx.builder.CreateLoad(
                valueType->getPointerElementType(),
                value
            );
        }
    }

    return value;
}

void emitPanic(const std::string& message, CodeGenContext& ctx) {
    // ─── Get the panic function from the runtime ──────────────────────────
    llvm::Function* panicFunc = ctx.getRuntimeFunction("__lucid_panic");
    if (!panicFunc) {
        // Declare the panic function
        llvm::FunctionType* panicType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {llvm::PointerType::get(ctx.llvmCtx, 0)}, // const char*
            false
        );
        panicFunc = llvm::Function::Create(
            panicType,
            llvm::Function::ExternalLinkage,
            "__lucid_panic",
            ctx.module
        );
        ctx.setRuntimeFunction("__lucid_panic", panicFunc);
    }

    // ─── Create a global string for the message ──────────────────────────
    llvm::Constant* msgConst = llvm::ConstantDataArray::getString(
        ctx.llvmCtx,
        message,
        true // Null terminate
    );

    llvm::GlobalVariable* msgGlobal = new llvm::GlobalVariable(
        *ctx.module,
        msgConst->getType(),
        true, // const
        llvm::GlobalValue::PrivateLinkage,
        msgConst,
        "panic_msg"
    );

    // ─── Get a pointer to the string ──────────────────────────────────────
    llvm::Value* msgPtr = ctx.builder.CreatePointerCast(
        msgGlobal,
        llvm::PointerType::get(ctx.llvmCtx, 0)
    );

    // ─── Call panic ───────────────────────────────────────────────────────
    ctx.builder.CreateCall(panicFunc, {msgPtr});

    // ─── Panic should not return - emit unreachable ──────────────────────
    ctx.builder.CreateUnreachable();
}

} // namespace codegen