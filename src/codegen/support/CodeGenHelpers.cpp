#include "CodeGenHelpers.hpp"
#include "CodeGenAlloca.hpp"
#include "../types/LLVMTypeHelpers.hpp"
#include "codegen/CodeGen.hpp"
#include "core/ast/ExprAST.hpp"

namespace codegen {

llvm::Value* getArrayLength(llvm::Value* target, ArrayTypeAST* arrayType, CodeGenContext& ctx) {
    if (!target || !arrayType) return nullptr;

    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx.llvmCtx);

    switch (arrayType->arrayKind) {
        case ArrayKind::Fixed: {
            // Fixed array: length is a compile-time constant
            return llvm::ConstantInt::get(i64Ty, arrayType->size);
        }

        case ArrayKind::Dynamic: {
            // Dynamic array: length is stored before the data
            // The target is a pointer to the array data.
            // The length is stored at offset -8 from the data pointer.
            if (target->getType()->isPointerTy()) {
                // Get the length by reading from the allocation header
                // In the runtime, dynamic arrays are allocated with a header:
                // [length: i64][data: T*]
                llvm::Value* lenPtr = ctx.builder.CreatePtrToInt(target, i64Ty, "data_ptr_int");
                lenPtr = ctx.builder.CreateSub(lenPtr, llvm::ConstantInt::get(i64Ty, 8), "len_ptr_int");
                lenPtr = ctx.builder.CreateIntToPtr(lenPtr, llvm::PointerType::get(ctx.llvmCtx, 0), "len_ptr");
                return ctx.builder.CreateLoad(i64Ty, lenPtr, "array_len");
            }
            
            // Fallback: return 0
            ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, SourceLocation(),
                                      "dynamic array length extraction not fully implemented");
            return llvm::ConstantInt::get(i64Ty, 0);
        }

        case ArrayKind::Slice: {
            // Slice: { ptr, len, cap }
            // The target is the slice struct value.
            // Extract the len field (index 1).
            if (target->getType()->isStructTy()) {
                llvm::StructType* structType = llvm::cast<llvm::StructType>(target->getType());
                if (structType->getNumElements() > 1) {
                    llvm::Value* len = ctx.builder.CreateExtractValue(target, 1, "slice_len");
                    return len;
                }
            }
            
            // If target is a pointer to a slice struct:
            if (target->getType()->isPointerTy()) {
                // GEP to the len field using an i8* base
                llvm::Value* base = ctx.builder.CreatePointerCast(
                    target,
                    llvm::PointerType::get(ctx.llvmCtx, 0),
                    "slice_base"
                );
                llvm::Value* lenPtr = ctx.builder.CreateConstGEP1_32(
                    llvm::Type::getInt8Ty(ctx.llvmCtx),
                    base,
                    8,  // offset of len field after ptr (assuming ptr is 8 bytes)
                    "slice_len_ptr"
                );
                return ctx.builder.CreateLoad(i64Ty, lenPtr, "slice_len");
            }
            
            ctx.diagnostics.warningAt(DiagCode::Warn_UnreachableCode, SourceLocation(),
                                      "slice length extraction not fully implemented");
            return llvm::ConstantInt::get(i64Ty, 0);
        }

        default:
            return nullptr;
    }
}

}