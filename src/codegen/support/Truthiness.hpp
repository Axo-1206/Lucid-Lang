#pragma once

#include "../types/CodeGenType.hpp"
#include "CodeGenHelpers.hpp"
#include "../types/LLVMTypeHelpers.hpp"
#include "core/ast/TypeAST.hpp"

namespace codegen {

/// Convert a lowered Lucid value into an LLVM i1 truth value.
inline llvm::Value* emitTruthiness(llvm::Value* value, TypeAST* type,
                                   CodeGenContext& ctx) {
    if (!value || !type) return value;

    llvm::Type* valueType = value->getType();
    llvm::Type* boolType = llvm::Type::getInt1Ty(ctx.llvmCtx);

    if (type->isa<CombinedTypeAST>()) {
        llvm::Value* tag = ctx.builder.CreateExtractValue(value, 0, "truth_tag");
        return ctx.builder.CreateICmpEQ(
            tag, llvm::ConstantInt::get(tag->getType(), 1), "truth_value");
    }

    if (type->isa<NullableTypeAST>()) {
        llvm::Value* tag = ctx.builder.CreateExtractValue(value, 0, "truth_tag");
        return ctx.builder.CreateICmpNE(
            tag, llvm::ConstantInt::get(tag->getType(), 0), "truth_not_nil");
    }

    if (type->isa<FallibleTypeAST>()) {
        llvm::Value* tag = ctx.builder.CreateExtractValue(value, 0, "truth_tag");
        return ctx.builder.CreateICmpNE(
            tag, llvm::ConstantInt::get(tag->getType(), 2), "truth_not_err");
    }

    if (type->isa<PrimitiveTypeAST>()) {
        auto* primitive = type->as<PrimitiveTypeAST>();
        switch (primitive->primitiveKind) {
            case PrimitiveKind::Bool:
                return value;
            case PrimitiveKind::String:
                if (valueType->isStructTy()) {
                    llvm::Value* length = ctx.builder.CreateExtractValue(value, 1,
                                                                           "str_len");
                    return ctx.builder.CreateICmpNE(
                        length, llvm::ConstantInt::get(length->getType(), 0),
                        "str_not_empty");
                }
                return ctx.builder.CreateICmpNE(
                    value, llvm::Constant::getNullValue(valueType), "str_not_null");
            case PrimitiveKind::Char:
                return ctx.builder.CreateICmpNE(
                    value, llvm::ConstantInt::get(valueType, 0), "char_nonzero");
            default:
                break;
        }

        if (isIntegerKind(primitive->primitiveKind)) {
            return ctx.builder.CreateICmpNE(
                value, llvm::Constant::getNullValue(valueType), "int_nonzero");
        }

        if (isFloatKind(primitive->primitiveKind)) {
            return ctx.builder.CreateFCmpONE(
                value, llvm::ConstantFP::get(valueType, 0.0), "float_nonzero");
        }
    }

    if (type->isa<ArrayTypeAST>()) {
        auto* array = type->as<ArrayTypeAST>();
        llvm::Value* length = getArrayLength(value, array, ctx);
        if (length) {
            return ctx.builder.CreateICmpNE(
                length, llvm::ConstantInt::get(length->getType(), 0), "array_not_empty");
        }
    }

    // Named values and other concrete values are always truthy. Pointers use
    // nullness as their natural fallback.
    if (type->isa<NamedTypeAST>()) {
        return llvm::ConstantInt::get(boolType, 1);
    }
    if (valueType->isPointerTy()) {
        return ctx.builder.CreateICmpNE(
            value, llvm::Constant::getNullValue(valueType), "truth_not_null");
    }
    return llvm::ConstantInt::get(boolType, 1);
}

} // namespace codegen
