/// @file LucidIntrinsicEmitter.cpp
/// @brief Implementation of Lucid-specific intrinsic emissions.

#include "LucidIntrinsicEmitter.hpp"
#include "../CodeGenType.hpp"
#include "../support/CodeGenAlloca.hpp"
#include "../support/CodeGenPanic.hpp"
#include "../support/LLVMHelpers.hpp"

#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/DerivedTypes.h>

#include <unordered_set>

namespace codegen {

// ─── Helper: Get type name as string ────────────────────────────────────

static std::string getLucidTypeName(CodeGenContext& ctx, const TypeAST* type) {
    if (!type) return "unknown";
    return getTypeName(ctx, type);
}

// ─── Type Inspection Intrinsics ──────────────────────────────────────────

llvm::Value* emitLucidTypeIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);

    // ─── #sizeof(T) ──────────────────────────────────────────────────────
    if (name == "sizeof") {
        if (expr && expr->resolvedType) {
            llvm::Type* llvmType = getType(ctx, expr->resolvedType);
            if (llvmType) {
                uint64_t size = ctx.getTypeSize(llvmType);
                return llvm::ConstantInt::get(i64, size);
            }
        }
        return llvm::ConstantInt::get(i64, 0);
    }

    // ─── #alignof(T) ──────────────────────────────────────────────────────
    if (name == "alignof") {
        if (expr && expr->resolvedType) {
            llvm::Type* llvmType = getType(ctx, expr->resolvedType);
            if (llvmType) {
                uint64_t alignment = ctx.getTypeAlign(llvmType);
                return llvm::ConstantInt::get(i64, alignment);
            }
        }
        return llvm::ConstantInt::get(i64, 0);
    }

    // ─── #bitcast(T, x) ──────────────────────────────────────────────────
    if (name == "bitcast") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#bitcast' requires an argument");
            return nullptr;
        }

        llvm::Type* targetType = getType(ctx, expr->resolvedType);
        if (!targetType) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "could not determine target type for '#bitcast'");
            return nullptr;
        }

        llvm::Value* val = args[0];
        return ctx.builder.CreateBitCast(val, targetType);
    }

    // ─── #typeof(x) ──────────────────────────────────────────────────────
    if (name == "typeof") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#typeof' requires an argument");
            return nullptr;
        }

        const TypeAST* type = expr->args[0]->resolvedType;
        if (!type) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "could not determine type for '#typeof'");
            return nullptr;
        }

        std::string typeName = getLucidTypeName(ctx, type);
        return ctx.createStringLiteral(typeName);
    }

    // ─── #nameof(x) ──────────────────────────────────────────────────────
    if (name == "nameof") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#nameof' requires an argument");
            return nullptr;
        }

        std::string nameStr;
        ExprAST* arg = expr->args[0];
        if (auto* ident = llvm::dyn_cast<IdentifierExprAST>(arg)) {
            nameStr = ctx.pool.lookup(ident->name);
        } else if (auto* field = llvm::dyn_cast<FieldAccessExprAST>(arg)) {
            nameStr = ctx.pool.lookup(field->fieldName);
        } else {
            nameStr = "unknown";
        }

        return ctx.createStringLiteral(nameStr);
    }

    // ─── #tostr(x) ──────────────────────────────────────────────────────
    if (name == "tostr") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#tostr' requires an argument");
            return nullptr;
        }

        // TODO: Implement proper to-string conversion
        return llvm::Constant::getNullValue(ctx.getStringType());
    }

    // ─── #ptrstr(x) ──────────────────────────────────────────────────────
    if (name == "ptrstr") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#ptrstr' requires an argument");
            return nullptr;
        }

        // TODO: Implement proper pointer-to-string conversion
        return llvm::Constant::getNullValue(ctx.getStringType());
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown type intrinsic '#", name, "'");
    return nullptr;
}

// ─── Pointer Intrinsics ──────────────────────────────────────────────────

llvm::Value* emitLucidPointerIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── toPtr(ref) ──────────────────────────────────────────────────────
    if (name == "toPtr") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#toPtr' requires an argument");
            return nullptr;
        }
        return args[0];
    }

    // ─── ptrOffset(ptr, n) ──────────────────────────────────────────────
    if (name == "ptrOffset") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#ptrOffset' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Value* offset = args[1];

        llvm::Type* elemType = ctx.getPointeeType(ptr);
        llvm::Value* gep = ctx.builder.CreateInBoundsGEP(
            elemType,
            ptr,
            offset,
            "ptr_offset"
        );

        return gep;
    }

    // ─── ptrDiff(p1, p2) ──────────────────────────────────────────────
    if (name == "ptrDiff") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#ptrDiff' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* p1 = ctx.builder.CreatePtrToInt(
            args[0],
            llvm::Type::getInt64Ty(ctx.llvmCtx)
        );
        llvm::Value* p2 = ctx.builder.CreatePtrToInt(
            args[1],
            llvm::Type::getInt64Ty(ctx.llvmCtx)
        );

        llvm::Value* diffBytes = ctx.builder.CreateSub(p1, p2, "ptr_diff_bytes");

        llvm::Type* elemType = ctx.getPointeeType(args[0]);
        uint64_t elemSize = ctx.getTypeSize(elemType);

        if (elemSize > 1) {
            llvm::Value* elemSizeVal = llvm::ConstantInt::get(
                llvm::Type::getInt64Ty(ctx.llvmCtx),
                elemSize
            );
            return ctx.builder.CreateSDiv(diffBytes, elemSizeVal, "ptr_diff_elements");
        }

        return diffBytes;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown pointer intrinsic '#", name, "'");
    return nullptr;
}

// ─── Memory Management Intrinsics ────────────────────────────────────────

llvm::Value* emitLucidMemoryMgmtIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);
    llvm::Type* i8Ptr = llvm::PointerType::get(ctx.llvmCtx, 0);

    // ─── #alloc(T, count) -> *T ──────────────────────────────────────
    if (name == "alloc") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#alloc' requires 2 arguments (type, count)");
            return nullptr;
        }

        llvm::FunctionType* allocType = llvm::FunctionType::get(
            i8Ptr,
            {i64},
            false
        );
        llvm::Function* allocFunc = ctx.getOrCreateRuntimeFunction("__lucid_alloc", allocType);

        llvm::Value* size = args[1];
        llvm::Value* result = ctx.builder.CreateCall(allocFunc, {size});

        llvm::Type* targetType = getType(ctx, expr->resolvedType);
        if (targetType && targetType->isPointerTy()) {
            return ctx.builder.CreateBitCast(result, targetType);
        }
        return result;
    }

    // ─── #free(ptr) ──────────────────────────────────────────────────────
    if (name == "free") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#free' requires an argument");
            return nullptr;
        }

        llvm::FunctionType* freeType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {i8Ptr},
            false
        );
        llvm::Function* freeFunc = ctx.getOrCreateRuntimeFunction("__lucid_free", freeType);

        llvm::Value* ptr = args[0];
        if (ptr->getType() != i8Ptr) {
            ptr = ctx.builder.CreateBitCast(ptr, i8Ptr);
        }
        ctx.builder.CreateCall(freeFunc, {ptr});
        return nullptr;
    }

    // ─── #arena_create(size) -> ArenaDescriptor ─────────────────────────
    if (name == "arena_create") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#arena_create' requires an argument");
            return nullptr;
        }

        llvm::Type* arenaType = llvm::StructType::get(ctx.llvmCtx, {i8Ptr, i64});

        llvm::FunctionType* arenaCreateType = llvm::FunctionType::get(
            arenaType,
            {i64},
            false
        );
        llvm::Function* arenaCreateFunc = ctx.getOrCreateRuntimeFunction(
            "__lucid_arena_create", arenaCreateType
        );

        return ctx.builder.CreateCall(arenaCreateFunc, {args[0]});
    }

    // ─── #arena_alloc(arena, T, n) -> *T ──────────────────────────────
    if (name == "arena_alloc") {
        if (args.size() < 3) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#arena_alloc' requires 3 arguments");
            return nullptr;
        }

        llvm::FunctionType* arenaAllocType = llvm::FunctionType::get(
            i8Ptr,
            {i8Ptr, i64},
            false
        );
        llvm::Function* arenaAllocFunc = ctx.getOrCreateRuntimeFunction(
            "__lucid_arena_alloc", arenaAllocType
        );

        llvm::Value* arena = args[0];
        llvm::Value* count = args[2];

        llvm::Value* result = ctx.builder.CreateCall(arenaAllocFunc, {arena, count});

        llvm::Type* targetType = getType(ctx, expr->resolvedType);
        if (targetType && targetType->isPointerTy()) {
            return ctx.builder.CreateBitCast(result, targetType);
        }
        return result;
    }

    // ─── #arena_reset(arena) ─────────────────────────────────────────────
    if (name == "arena_reset") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#arena_reset' requires an argument");
            return nullptr;
        }

        llvm::FunctionType* arenaResetType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {i8Ptr},
            false
        );
        llvm::Function* arenaResetFunc = ctx.getOrCreateRuntimeFunction(
            "__lucid_arena_reset", arenaResetType
        );

        llvm::Value* arena = args[0];
        if (arena->getType() != i8Ptr) {
            arena = ctx.builder.CreateBitCast(arena, i8Ptr);
        }
        ctx.builder.CreateCall(arenaResetFunc, {arena});
        return nullptr;
    }

    // ─── #arena_free(arena) ──────────────────────────────────────────────
    if (name == "arena_free") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#arena_free' requires an argument");
            return nullptr;
        }

        llvm::FunctionType* arenaFreeType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(ctx.llvmCtx),
            {i8Ptr},
            false
        );
        llvm::Function* arenaFreeFunc = ctx.getOrCreateRuntimeFunction(
            "__lucid_arena_free", arenaFreeType
        );

        llvm::Value* arena = args[0];
        if (arena->getType() != i8Ptr) {
            arena = ctx.builder.CreateBitCast(arena, i8Ptr);
        }
        ctx.builder.CreateCall(arenaFreeFunc, {arena});
        return nullptr;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown memory management intrinsic '#", name, "'");
    return nullptr;
}

// ─── String Intrinsics ────────────────────────────────────────────────────

llvm::Value* emitLucidStringIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    llvm::Type* strType = ctx.getStringType();
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);

    // ─── str_len(s) ──────────────────────────────────────────────────────
    if (name == "str_len") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_len' requires a string argument");
            return nullptr;
        }

        llvm::Value* str = args[0];
        return ctx.builder.CreateExtractValue(str, 1, "str_len");
    }

    // ─── str_ptr(s) ──────────────────────────────────────────────────────
    if (name == "str_ptr") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_ptr' requires a string argument");
            return nullptr;
        }

        llvm::Value* str = args[0];
        return ctx.builder.CreateExtractValue(str, 0, "str_ptr");
    }

    // ─── str_from_ptr(ptr, len) ──────────────────────────────────────────
    if (name == "str_from_ptr") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_from_ptr' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Value* len = args[1];

        llvm::Value* str = llvm::UndefValue::get(strType);
        str = ctx.builder.CreateInsertValue(str, ptr, 0);
        str = ctx.builder.CreateInsertValue(str, len, 1);
        str = ctx.builder.CreateInsertValue(str, len, 2);
        return str;
    }

    // ─── str_concat(a, b) ─────────────────────────────────────────────────
    if (name == "str_concat") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_concat' requires 2 arguments");
            return nullptr;
        }

        llvm::FunctionType* concatType = llvm::FunctionType::get(
            strType,
            {strType, strType},
            false
        );
        llvm::Function* concatFunc = ctx.getOrCreateRuntimeFunction(
            "__lucid_str_concat", concatType
        );

        return ctx.builder.CreateCall(concatFunc, {args[0], args[1]});
    }

    // ─── str_slice(s, from, to) ──────────────────────────────────────────
    if (name == "str_slice") {
        if (args.size() < 3) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_slice' requires 3 arguments");
            return nullptr;
        }

        llvm::FunctionType* sliceType = llvm::FunctionType::get(
            strType,
            {strType, i64, i64},
            false
        );
        llvm::Function* sliceFunc = ctx.getOrCreateRuntimeFunction(
            "__lucid_str_slice", sliceType
        );

        return ctx.builder.CreateCall(sliceFunc, {args[0], args[1], args[2]});
    }

    // ─── str_eq(a, b) ─────────────────────────────────────────────────────
    if (name == "str_eq") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_eq' requires 2 arguments");
            return nullptr;
        }

        llvm::FunctionType* eqType = llvm::FunctionType::get(
            llvm::Type::getInt1Ty(ctx.llvmCtx),
            {strType, strType},
            false
        );
        llvm::Function* eqFunc = ctx.getOrCreateRuntimeFunction(
            "__lucid_str_eq", eqType
        );

        return ctx.builder.CreateCall(eqFunc, {args[0], args[1]});
    }

    // ─── str_byte_at(s, i) ──────────────────────────────────────────────
    if (name == "str_byte_at") {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_byte_at' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* str = args[0];
        llvm::Value* idx = args[1];
        llvm::Value* ptr = ctx.builder.CreateExtractValue(str, 0);

        llvm::Value* bytePtr = ctx.builder.CreateGEP(
            llvm::Type::getInt8Ty(ctx.llvmCtx),
            ptr,
            idx,
            "str_byte_ptr"
        );

        return ctx.builder.CreateLoad(llvm::Type::getInt8Ty(ctx.llvmCtx), bytePtr);
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown string intrinsic '#", name, "'");
    return nullptr;
}

// ─── Control Flow Intrinsics ─────────────────────────────────────────────

llvm::Value* emitLucidControlIntrinsic(
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    const IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── scope_exit ──────────────────────────────────────────────────────
    if (name == "scope_exit") {
        // scope_exit is handled in Sema and stored on BlockStmtAST.
        // CodeGenStmt.cpp emits these callbacks.
        // No runtime code is generated at the call site.
        return nullptr;
    }

    // ─── likely / unlikely ──────────────────────────────────────────────
    if (name == "likely" || name == "unlikely") {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#", name, "' requires an argument");
            return nullptr;
        }

        // Return the condition value - branch weight metadata will be added later
        return args[0];
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown control intrinsic '#", name, "'");
    return nullptr;
}

} // namespace codegen