/// @file CodeGenHelpers.cpp
/// @brief Implementation of code generation helper functions.

#include "CodeGenHelpers.hpp"
#include "../CodeGenType.hpp"
#include "debug/DebugUtils.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>

namespace codegen {

// ─── Alloca Creation ──────────────────────────────────────────────────────

llvm::AllocaInst* createAlloca(
    const std::string& name,
    llvm::Type* type,
    CodeGenContext& ctx
) {
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        return nullptr;
    }

    llvm::BasicBlock* entryBlock = &func->getEntryBlock();
    llvm::IRBuilder<> builder(ctx.llvmCtx);
    builder.SetInsertPoint(entryBlock, entryBlock->getFirstInsertionPt());

    return builder.CreateAlloca(type, nullptr, name);
}

llvm::BasicBlock* createBlock(
    const std::string& name,
    CodeGenContext& ctx
) {
    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) {
        return nullptr;
    }

    return llvm::BasicBlock::Create(ctx.llvmCtx, name, func);
}

// ─── Load Helpers ─────────────────────────────────────────────────────────

llvm::Value* loadIfNeeded(
    llvm::Value* value,
    llvm::Type* elemType,
    CodeGenContext& ctx
) {
    if (!value || !elemType) return value;

    if (value->getType()->isPointerTy()) {
        return ctx.builder.CreateLoad(elemType, value);
    }

    return value;
}

llvm::Value* loadIfNeeded(
    llvm::Value* value,
    bool isLValue,
    CodeGenContext& ctx
) {
    if (!value || !isLValue) return value;

    ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, SourceLocation(),
                              "loadIfNeeded(bool) is deprecated - use loadIfNeeded(value, elemType, ctx)");
    return value;
}

// ─── Panic ─────────────────────────────────────────────────────────────────

void emitPanic(
    const std::string& message,
    CodeGenContext& ctx
) {
    llvm::Function* panicFunc = ctx.getRuntimeFunction("__lucid_panic");
    if (!panicFunc) {
        llvm::FunctionType* panicType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {llvm::PointerType::get(ctx.llvmCtx, 0)},
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

    llvm::Constant* msgConst = llvm::ConstantDataArray::getString(
        ctx.llvmCtx,
        message,
        true
    );

    llvm::GlobalVariable* msgGlobal = new llvm::GlobalVariable(
        *ctx.module,
        msgConst->getType(),
        true,
        llvm::GlobalValue::PrivateLinkage,
        msgConst,
        "panic_msg"
    );

    llvm::Value* msgPtr = ctx.builder.CreatePointerCast(
        msgGlobal,
        llvm::PointerType::get(ctx.llvmCtx, 0)
    );

    ctx.builder.CreateCall(panicFunc, {msgPtr});
    ctx.builder.CreateUnreachable();
}

llvm::Value* emitNullCheck(
    llvm::Value* ptr,
    const std::string& message,
    CodeGenContext& ctx
) {
    if (!ptr) return nullptr;

    llvm::Type* ptrType = ptr->getType();
    if (!ptrType->isPointerTy()) return ptr;

    llvm::Function* func = ctx.getCurrentFunction();
    if (!func) return ptr;

    llvm::Value* isNull = ctx.builder.CreateICmpEQ(
        ptr,
        llvm::Constant::getNullValue(ptrType),
        "ptr_is_null"
    );

    llvm::BasicBlock* checkBlock = ctx.builder.GetInsertBlock();
    llvm::BasicBlock* passBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "null_check_pass",
        func
    );
    llvm::BasicBlock* failBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "null_check_fail",
        func
    );
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
        ctx.llvmCtx,
        "null_check_merge",
        func
    );

    ctx.builder.CreateCondBr(isNull, failBlock, passBlock);

    ctx.builder.SetInsertPoint(passBlock);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(failBlock);
    emitPanic(message, ctx);
    ctx.builder.CreateBr(mergeBlock);

    ctx.builder.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = ctx.builder.CreatePHI(ptrType, 2, "null_check_result");
    phi->addIncoming(ptr, passBlock);
    phi->addIncoming(llvm::Constant::getNullValue(ptrType), failBlock);

    return phi;
}

// ─── Type Helpers ─────────────────────────────────────────────────────────

llvm::Type* getDeclType(
    const ValueDeclAST* decl,
    CodeGenContext& ctx
) {
    if (!decl) return nullptr;
    return getType(ctx, decl->type);
}

// ─── Generic Helper Functions ────────────────────────────────────────────

bool isGenericFunction(const FuncDeclAST* decl) {
    return decl && !decl->genericParams.empty();
}

bool isGenericStruct(const StructDeclAST* decl) {
    return decl && !decl->genericParams.empty();
}

bool shouldSpecialize(const DeclAST* decl) {
    if (!decl) return false;

    if (decl->isa<FuncDeclAST>()) {
        return decl->as<FuncDeclAST>()->shouldSpecialize;
    }
    if (decl->isa<StructDeclAST>()) {
        return decl->as<StructDeclAST>()->shouldSpecialize;
    }
    return false;
}

bool isGenericParameterName(InternedString name, const ArenaSpan<GenericParamDeclPtr>& genericParams) {
    for (const GenericParamDeclPtr param : genericParams) {
        if (param->name == name) {
            return true;
        }
    }
    return false;
}

size_t findGenericParamIndex(InternedString name, const ArenaSpan<GenericParamDeclPtr>& genericParams) {
    for (size_t i = 0; i < genericParams.size(); ++i) {
        if (genericParams[i]->name == name) {
            return i;
        }
    }
    return SIZE_MAX;
}

const TypeAST* substituteGenericType(
    const TypeAST* type,
    const ArenaSpan<GenericParamDeclPtr>& genericParams,
    const std::vector<const TypeAST*>& typeArgs,
    CodeGenContext& ctx
) {
    if (!type) return nullptr;
    (void)ctx;

    if (type->isa<NamedTypeAST>()) {
        const NamedTypeAST* named = type->as<NamedTypeAST>();
        size_t index = findGenericParamIndex(named->name, genericParams);
        if (index != SIZE_MAX && index < typeArgs.size()) {
            return typeArgs[index];
        }
        return type;
    }

    if (type->isa<ArrayTypeAST>()) {
        const ArrayTypeAST* arr = type->as<ArrayTypeAST>();
        const TypeAST* substitutedElement = substituteGenericType(
            arr->element,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedElement != arr->element) {
            // TODO: Use arena to create new type nodes
        }
        return type;
    }

    if (type->isa<NullableTypeAST>()) {
        const NullableTypeAST* nullable = type->as<NullableTypeAST>();
        const TypeAST* substitutedInner = substituteGenericType(
            nullable->inner,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedInner != nullable->inner) {
            // TODO: Use arena to create new type nodes
        }
        return type;
    }

    if (type->isa<FallibleTypeAST>()) {
        const FallibleTypeAST* fallible = type->as<FallibleTypeAST>();
        const TypeAST* substitutedInner = substituteGenericType(
            fallible->inner,
            genericParams,
            typeArgs,
            ctx
        );
        if (substitutedInner != fallible->inner) {
            // TODO: Use arena to create new type nodes
        }
        return type;
    }

    return type;
}

} // namespace codegen