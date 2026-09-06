/// @file CodeGenDecl.cpp
/// @brief Implementation of declaration lowering to LLVM IR.
///
/// This file handles lowering of all declarations (functions, variables,
/// structs, enums) to LLVM IR. It operates in two phases to support
/// forward references.
///
/// ─── Two-Phase Design ──────────────────────────────────────────────────────
///   Phase 1 (lowerModuleDeclarations): Create all prototypes/types.
///   Phase 2 (lowerModuleBodies): Generate function bodies.
///
/// ─── Generic Function Strategy ────────────────────────────────────────────
///   1. DEFAULT (Type Erasure): One erased function with tagged slots.
///   2. OPT-IN (@[specialize]): One specialized function per instantiation.

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
// 1. Declaration Dispatch
// =============================================================================

void lowerDeclaration(DeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    switch (decl->kind) {
        case ASTKind::ImportDecl: break;
        case ASTKind::FuncDecl:   lowerFunctionDecl(decl->as<FuncDeclAST>(), ctx); break;
        case ASTKind::StructDecl: lowerStructDecl(decl->as<StructDeclAST>(), ctx); break;
        case ASTKind::EnumDecl:   lowerEnumDecl(decl->as<EnumDeclAST>(), ctx); break;
        case ASTKind::VarDecl:    lowerVarDecl(decl->as<VarDeclAST>(), ctx); break;
        default: break;
    }
}

// =============================================================================
// 2. Function Declaration (Phase 1)
// =============================================================================

void lowerFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;
    if (ctx.lookupFunction(decl)) return;

    // ─── Foreign functions ──────────────────────────────────────────────────
    if (decl->isForeignFunction) {
        lowerForeignFunctionDecl(decl, ctx);
        return;
    }

    // ─── Generic functions ─────────────────────────────────────────────────
    if (isGenericFunction(decl)) {
        lowerGenericFunctionDecl(decl, ctx);
        return;
    }

    // ─── Non-generic functions ─────────────────────────────────────────────
    lowerNormalFunctionDecl(decl, ctx);
}

// ─── 2.1 Foreign Functions ─────────────────────────────────────────────────

void lowerForeignFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    llvm::FunctionType* funcType = getFunctionType(ctx, decl->funcType, decl->hasClosure);
    std::string funcName = ctx.pool.lookup(decl->name);
    llvm::Function* func = llvm::Function::Create(
        funcType,
        llvm::Function::ExternalLinkage,
        funcName,
        ctx.module
    );
    ctx.storeFunction(decl, func);
    decl->llvmFunction = func;

    Trace::detail("Lowered foreign function: ", funcName);
}

// ─── 2.2 Generic Functions ─────────────────────────────────────────────────

void lowerGenericFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (shouldSpecialize(decl)) {
        // @[specialize]: Lazily generated on first use
        Trace::detail("Registered specialized generic: ", ctx.pool.lookup(decl->name));
        return;
    }

    // Default: Type-erased function with tagged slots
    llvm::Function* func = generateErasedGenericFunction(decl, ctx);
    if (func) {
        ctx.storeFunction(decl, func);
        decl->llvmFunction = func;
        Trace::detail("Created erased generic prototype: ", func->getName().str());
    }
}

// ─── 2.3 Normal (Non-Generic) Functions ──────────────────────────────────

void lowerNormalFunctionDecl(FuncDeclAST* decl, CodeGenContext& ctx) {
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
    if (decl->hasClosure) {
        func->getArg(paramIndex++)->setName("env");
    }
    for (ParamAST* param : decl->funcType->params) {
        if (paramIndex < func->arg_size()) {
            func->getArg(paramIndex)->setName(ctx.pool.lookup(param->name));
            paramIndex++;
        }
    }

    ctx.storeFunction(decl, func);
    decl->llvmFunction = func;
    ctx.module->getOrInsertFunction(funcName, funcType);

    // ─── Track mutable closure functions ──────────────────────────────────
    trackClosureFunction(decl, func, ctx);

    Trace::detail("Lowered function declaration: ", funcName,
                " (", func->arg_size(), " params)");
}

// ─── 2.4 Track Mutable Closure Functions ──────────────────────────────────

void trackClosureFunction(FuncDeclAST* decl, llvm::Function* func, CodeGenContext& ctx) {
    if (decl->keyword != DeclKeyword::Let || !decl->hasClosure) return;

    ctx.markAlive(decl);

    llvm::Type* closureType = ctx.getClosureType();
    llvm::AllocaInst* alloca = createAlloca(
        ctx.pool.lookup(decl->name) + "_closure",
        closureType,
        ctx
    );

    llvm::Value* closureVal = llvm::UndefValue::get(closureType);
    llvm::Value* funcPtr = ctx.builder.CreatePointerCast(
        func,
        llvm::PointerType::get(ctx.llvmCtx, 0),
        "func_ptr"
    );
    closureVal = ctx.builder.CreateInsertValue(closureVal, funcPtr, 0);
    closureVal = ctx.builder.CreateInsertValue(
        closureVal,
        llvm::ConstantPointerNull::get(llvm::PointerType::get(ctx.llvmCtx, 0)),
        1
    );

    ctx.builder.CreateStore(closureVal, alloca);
    ctx.storeValue(decl, alloca);

    Trace::detail("Tracked mutable closure: ", ctx.pool.lookup(decl->name));
}

// =============================================================================
// 3. Function Body (Phase 2)
// =============================================================================

void lowerFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;
    if (decl->isForeignFunction) return;

    // ─── Generic functions ─────────────────────────────────────────────────
    if (isGenericFunction(decl)) {
        lowerGenericFunctionBody(decl, ctx);
        return;
    }

    // ─── Non-generic functions ─────────────────────────────────────────────
    lowerNormalFunctionBody(decl, ctx);
}

// ─── 3.1 Generic Function Bodies ──────────────────────────────────────────

void lowerGenericFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    if (shouldSpecialize(decl)) {
        // @[specialize]: Generated lazily on instantiation
        Trace::detail("Specialized generic body deferred: ",
                      ctx.pool.lookup(decl->name));
        return;
    }

    // Default: Type-erased function with tagged slots
    llvm::Function* func = ctx.lookupFunction(decl);
    if (!func) {
        ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, decl->loc,
            "erased generic function '", ctx.pool.lookup(decl->name),
            "' not created in Phase 1");
        return;
    }

    if (!func->empty()) return;
    lowerErasedFunctionBody(decl, func, ctx);
}

// ─── 3.2 Normal Function Bodies ───────────────────────────────────────────

void lowerNormalFunctionBody(FuncDeclAST* decl, CodeGenContext& ctx) {
    llvm::Function* func = ctx.lookupFunction(decl);
    if (!func) {
        ctx.diagnostics.errorAt(DiagCode::Sem_UndefinedValue, decl->loc,
            "function '", ctx.pool.lookup(decl->name),
            "' not found in symbol table");
        return;
    }

    if (!func->empty()) return;
    lowerFunctionBodyInternal(decl, func, ctx);
}

// ─── 3.3 Erased Function Bodies (Type-Erased Generics) ────────────────────

void lowerErasedFunctionBody(
    FuncDeclAST* decl,
    llvm::Function* func,
    CodeGenContext& ctx
) {
    // ─── Get TaggedSlot type ──────────────────────────────────────────────
    static const char* slotName = "TaggedSlot";
    llvm::StructType* slotType = llvm::StructType::getTypeByName(ctx.llvmCtx, slotName);
    if (!slotType) {
        std::vector<llvm::Type*> slotFields = {
            llvm::Type::getInt8Ty(ctx.llvmCtx),
            llvm::PointerType::get(ctx.llvmCtx, 0)
        };
        slotType = llvm::StructType::create(ctx.llvmCtx, slotFields, slotName);
    }

    ctx.setCurrentFunction(func);
    GenericSubstitution subst{decl->genericParams, {}};
    ctx.currentGenericSubstitution = &subst;

    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "entry", func);
    ctx.builder.SetInsertPoint(entryBlock);

    // ─── Unpack tagged slot parameters ────────────────────────────────────
    size_t argIndex = 0;

    if (decl->hasClosure) {
        ctx.currentEnvPtr = func->getArg(argIndex++);
        ctx.storeValue(nullptr, ctx.currentEnvPtr);
    }

    FuncTypeAST* funcType = decl->funcType;
    while (funcType) {
        for (ParamAST* param : funcType->params) {
            if (argIndex >= func->arg_size()) {
                ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, param->loc,
                    "too few arguments for erased function");
                ctx.setCurrentFunction(nullptr);
                ctx.currentGenericSubstitution = nullptr;
                return;
            }

            llvm::Value* slotPtr = func->getArg(argIndex++);
            slotPtr->setName(ctx.pool.lookup(param->name) + "_tagged");

            llvm::Value* slot = ctx.builder.CreateLoad(slotType, slotPtr,
                "slot_" + ctx.pool.lookup(param->name));
            llvm::Value* tag = ctx.builder.CreateExtractValue(slot, 0,
                "tag_" + ctx.pool.lookup(param->name));
            llvm::Value* value = ctx.builder.CreateExtractValue(slot, 1,
                "value_" + ctx.pool.lookup(param->name));

            llvm::Type* opaquePtrType = llvm::PointerType::get(ctx.llvmCtx, 0);
            llvm::AllocaInst* alloca = createAlloca(
                ctx.pool.lookup(param->name),
                opaquePtrType,
                ctx
            );
            ctx.builder.CreateStore(value, alloca);
            ctx.storeValue(param, alloca);
            param->llvmAlloca = alloca;
            param->llvmValue = value;
        }
        funcType = funcType->getNext();
    }

    // ─── Lower body ──────────────────────────────────────────────────────
    if (decl->body) {
        lowerStatement(decl->body, ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, decl->loc,
            "function '", ctx.pool.lookup(decl->name), "' has no body");
    }

    ctx.setCurrentFunction(nullptr);
    ctx.currentEnvPtr = nullptr;
    ctx.currentGenericSubstitution = nullptr;

    verifyFunction(func, decl->loc, ctx);
    Trace::detail("Lowered erased generic body: ", func->getName().str());
}

// ─── 3.4 Internal Function Body Lowering ──────────────────────────────────

void lowerFunctionBodyInternal(
    FuncDeclAST* decl,
    llvm::Function* func,
    CodeGenContext& ctx
) {
    ctx.setCurrentFunction(func);

    // Dummy substitution for generic parameter detection
    GenericSubstitution subst{decl->genericParams, {}};
    ctx.currentGenericSubstitution = &subst;

    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "entry", func);
    ctx.builder.SetInsertPoint(entryBlock);

    size_t argIndex = 0;

    if (decl->hasClosure) {
        ctx.currentEnvPtr = func->getArg(argIndex++);
        ctx.storeValue(nullptr, ctx.currentEnvPtr);
    }

    for (ParamAST* param : decl->funcType->params) {
        lowerParam(param, ctx);
        argIndex++;
    }

    if (decl->body) {
        lowerStatement(decl->body, ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Sem_MissingReturn, decl->loc,
            "function '", ctx.pool.lookup(decl->name), "' has no body");
    }

    ctx.setCurrentFunction(nullptr);
    ctx.currentEnvPtr = nullptr;
    ctx.currentGenericSubstitution = nullptr;

    verifyFunction(func, decl->loc, ctx);
    Trace::detail("Lowered function body: ", ctx.pool.lookup(decl->name));
}

// ─── 3.5 Specialized Function Bodies (@[specialize]) ──────────────────────

void lowerSpecializedFunctionBody(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    llvm::Function* specializedFunc,
    CodeGenContext& ctx
) {
    if (!funcDecl || !specializedFunc) return;

    auto savedValues = std::move(ctx.values);
    ctx.values.clear();

    GenericSubstitution subst{funcDecl->genericParams, typeArgs};
    ctx.currentGenericSubstitution = &subst;

    ctx.setCurrentFunction(specializedFunc);

    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(ctx.llvmCtx, "entry", specializedFunc);
    ctx.builder.SetInsertPoint(entryBlock);

    size_t argIndex = 0;

    if (funcDecl->hasClosure) {
        ctx.currentEnvPtr = specializedFunc->getArg(argIndex++);
    }

    for (ParamAST* param : funcDecl->funcType->params) {
        llvm::Type* llvmType = getType(ctx, param->type);
        if (!llvmType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
                "parameter '", ctx.pool.lookup(param->name),
                "' has invalid type in specialization");
            ctx.currentGenericSubstitution = nullptr;
            ctx.values = std::move(savedValues);
            return;
        }

        llvm::AllocaInst* alloca = createAlloca(
            ctx.pool.lookup(param->name),
            llvmType,
            ctx
        );
        llvm::Value* argValue = specializedFunc->getArg(argIndex);
        ctx.builder.CreateStore(argValue, alloca);
        ctx.storeValue(param, alloca);
        param->llvmAlloca = alloca;
        param->llvmValue = argValue;
        argIndex++;
    }

    if (funcDecl->body) {
        lowerStatement(funcDecl->body, ctx);
    } else {
        ctx.diagnostics.errorAt(DiagCode::Backend_CodegenError, funcDecl->loc,
            "specialized function '", specializedFunc->getName().str(),
            "' has no body");
    }

    ctx.setCurrentFunction(nullptr);
    ctx.currentEnvPtr = nullptr;
    ctx.currentGenericSubstitution = nullptr;
    ctx.values = std::move(savedValues);

    verifyFunction(specializedFunc, funcDecl->loc, ctx);
    Trace::detail("Lowered specialized body: ", specializedFunc->getName().str());
}

// =============================================================================
// 4. Parameter Lowering
// =============================================================================

void lowerParam(ParamAST* param, CodeGenContext& ctx) {
    if (!param) return;

    llvm::Type* paramType = nullptr;
    if (param->isVariadic) {
        paramType = ctx.getSliceType();
    } else if (param->type && param->type->isa<FuncTypeAST>()) {
        paramType = getFunctionRuntimeType(ctx, param->type->as<FuncTypeAST>(), true);
    } else {
        paramType = getType(ctx, param->type);
    }

    if (!paramType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
            "parameter '", ctx.pool.lookup(param->name), "' has invalid type");
        return;
    }

    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
            "parameter '", ctx.pool.lookup(param->name), "' has no current function");
        return;
    }

    llvm::Value* argValue = nullptr;
    for (auto& arg : func->args()) {
        if (arg.getName() == ctx.pool.lookup(param->name)) {
            argValue = &arg;
            break;
        }
    }

    if (!argValue) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, param->loc,
            "parameter '", ctx.pool.lookup(param->name), "' not found");
        return;
    }

    llvm::AllocaInst* alloca = createAlloca(ctx.pool.lookup(param->name), paramType, ctx);
    ctx.builder.CreateStore(argValue, alloca);
    ctx.storeValue(param, alloca);
    param->llvmAlloca = alloca;
    param->llvmValue = argValue;
}

// =============================================================================
// 5. Helper: Verify Function
// =============================================================================

void verifyFunction(llvm::Function* func, const SourceLocation& loc, CodeGenContext& ctx) {
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyFunction(*func, &errorStream)) {
        ctx.diagnostics.errorAt(DiagCode::Backend_InvalidIR, loc,
            "function '", func->getName().str(), "' failed verification: ", error);
    }
}

// =============================================================================
// 6. Variable Declaration
// =============================================================================

void lowerVarDecl(VarDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    llvm::Type* varType = getType(ctx, decl->type);
    if (!varType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, decl->loc,
            "variable '", ctx.pool.lookup(decl->name), "' has invalid type");
        return;
    }

    bool isModuleLevel = ctx.module && ctx.getCurrentFunction() == nullptr;

    if (isModuleLevel) {
        lowerGlobalVar(decl, varType, ctx);
    } else {
        lowerLocalVar(decl, varType, ctx);
    }
}

void lowerGlobalVar(VarDeclAST* decl, llvm::Type* varType, CodeGenContext& ctx) {
    std::string varName = decl->mangledName.isValid()
        ? ctx.pool.lookup(decl->mangledName)
        : ctx.pool.lookup(decl->name);

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

    if (decl->init) {
        if (decl->init->isConst) {
            llvm::Value* initValue = lowerExpression(decl->init, ctx);
            if (initValue) {
                if (llvm::Constant* constInit = llvm::dyn_cast<llvm::Constant>(initValue)) {
                    global->setInitializer(constInit);
                }
            }
        } else {
            ctx.pendingGlobals.push_back({
                decl, 
                decl->init, 
                global, 
                ctx.currentModule, 
                decl->orderInModule
            });
        }
    }
}

void lowerLocalVar(VarDeclAST* decl, llvm::Type* varType, CodeGenContext& ctx) {
    llvm::AllocaInst* alloca = createAlloca(ctx.pool.lookup(decl->name), varType, ctx);

    if (decl->init) {
        llvm::Value* initValue = lowerExpression(decl->init, ctx);
        if (initValue) {
            ctx.builder.CreateStore(initValue, alloca);
        }
    } else {
        ctx.builder.CreateStore(llvm::Constant::getNullValue(varType), alloca);
    }

    ctx.storeValue(decl, alloca);
    decl->llvmAlloca = alloca;

    // Mark alive if it owns heap memory
    if (decl->type) {
        bool needsCleanup = false;
        if (auto* array = decl->type->as<ArrayTypeAST>()) {
            needsCleanup = array->isDynamic();
        } else if (auto* prim = decl->type->as<PrimitiveTypeAST>()) {
            needsCleanup = (prim->primitiveKind == PrimitiveKind::String);
        }
        if (needsCleanup) ctx.markAlive(decl);
    }
}

// =============================================================================
// 7. Struct Declaration
// =============================================================================

void lowerStructDecl(StructDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;
    if (ctx.lookupStruct(decl)) return;

    if (isGenericStruct(decl)) {
        lowerGenericStructDecl(decl, ctx);
        return;
    }

    lowerNormalStructDecl(decl, ctx);
}

void lowerGenericStructDecl(StructDeclAST* decl, CodeGenContext& ctx) {
    if (shouldSpecialize(decl)) {
        // @[specialize]: Lazy generation
        Trace::detail("Registered generic struct template: ", ctx.pool.lookup(decl->name));
        return;
    }

    // Default: Type-erased struct with tagged slots
    llvm::Type* erasedType = generateErasedGenericStruct(decl, ctx);
    if (erasedType) {
        ctx.cacheStruct(decl, llvm::cast<llvm::StructType>(erasedType));
        decl->llvmType = llvm::cast<llvm::StructType>(erasedType);
        Trace::detail("Lowered type-erased generic struct: ", ctx.pool.lookup(decl->name));
    }
}

void lowerNormalStructDecl(StructDeclAST* decl, CodeGenContext& ctx) {
    if (!decl->mangledName.isValid()) {
        llvm_unreachable("Struct has no mangled name - Sema bug");
    }

    std::string structName = ctx.pool.lookup(decl->mangledName);
    llvm::StructType* structType = llvm::StructType::getTypeByName(ctx.llvmCtx, structName);

    if (!structType) {
        structType = llvm::StructType::create(ctx.llvmCtx, structName);
    }

    ctx.cacheStruct(decl, structType);

    std::vector<llvm::Type*> fieldTypes;
    for (FieldDeclAST* field : decl->fields) {
        llvm::Type* fieldType = nullptr;
        if (field->type && field->type->isa<FuncTypeAST>()) {
            fieldType = getFunctionRuntimeType(ctx, field->type->as<FuncTypeAST>(), true);
        } else {
            fieldType = getType(ctx, field->type);
        }
        if (!fieldType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, field->loc,
                "field '", ctx.pool.lookup(field->name), "' has invalid type");
            fieldType = llvm::Type::getInt8Ty(ctx.llvmCtx);
        }
        fieldTypes.push_back(fieldType);
    }

    if (structType->isOpaque()) {
        structType->setBody(fieldTypes);
    }

    ctx.cacheStruct(decl, structType);
    decl->llvmType = structType;

    Trace::detail("Lowered struct: ", structName, " (", fieldTypes.size(), " fields)");
}

// =============================================================================
// 8. Enum Declaration
// =============================================================================

void lowerEnumDecl(EnumDeclAST* decl, CodeGenContext& ctx) {
    if (!decl) return;

    llvm::IntegerType* backingType = getEnumType(ctx, decl);
    if (!backingType) {
        ctx.diagnostics.errorAt(DiagCode::Sem_InvalidParamType, decl->loc,
            "enum '", ctx.pool.lookup(decl->name), "' has invalid backing type");
        return;
    }

    std::string enumName = decl->mangledName.isValid()
        ? ctx.pool.lookup(decl->mangledName)
        : ctx.pool.lookup(decl->name);

    decl->variantConstants.clear();
    decl->variantConstants.reserve(decl->variants.size());

    for (EnumVariantAST* variant : decl->variants) {
        llvm::ConstantInt* constVal = llvm::ConstantInt::get(backingType, variant->value, true);
        decl->variantConstants.push_back(constVal);
        variant->llvmValue = constVal;

        std::string varName = enumName + "." + ctx.pool.lookup(variant->name);
        new llvm::GlobalVariable(*ctx.module, backingType, true,
            llvm::GlobalValue::ExternalLinkage, constVal, varName);

        Trace::detail("Lowered enum variant: ", varName, " = ", variant->value);
    }

    decl->backingLLVMType = backingType;
    Trace::detail("Lowered enum: ", enumName, " (", decl->variantConstants.size(), " variants)");
}

// =============================================================================
// 9. Specialized Function Body Instantiation (Public API)
// =============================================================================

void instantiateSpecializedFunctionBody(
    FuncDeclAST* funcDecl,
    const ArenaSpan<TypeAST*>& typeArgs,
    llvm::Function* specializedFunc,
    CodeGenContext& ctx
) {
    if (!funcDecl || !specializedFunc) return;
    if (!specializedFunc->empty()) return;

    lowerSpecializedFunctionBody(funcDecl, typeArgs, specializedFunc, ctx);
}

} // namespace codegen