/// @file support/ArenaHelpers.hpp
/// @brief Arena-specific code generation helpers.
///
/// ─── Purpose ──────────────────────────────────────────────────────────────────
/// This file provides helper functions for lowering arena operations in
/// CodeGenExpr.cpp. These helpers are small and header-only.
///
/// ─── Arena Grammar Rules ─────────────────────────────────────────────────────
/// According to the grammar, Arena is an "Owned, scope-confined" type with
/// no copy operation. It must be accessed through `&Arena` references:
///
///   const buildGraph (a &Arena) = {
///       let nodes [_]Node = a::alloc<Node>(128);
///   };
///
///   const run () = {
///       const arena Arena = Arena::create(4096) ?? Arena::empty();
///       buildGraph(arena);    // binds as &Arena
///   };
///
/// ─── Implementation Notes ────────────────────────────────────────────────────
/// - All runtime functions take i8* (opaque pointer) for Arena
/// - getArenaPointer() rejects by-value Arena (matches grammar)
/// - buildArenaDescriptor() builds { base, size } from an Arena

#pragma once

#include "../CodeGen.hpp"
#include "codegen/context/CodeGenContext.hpp"
#include "codegen/types/LLVMTypeHelpers.hpp"
#include "codegen/support/CodeGenHelpers.hpp"
#include "codegen/support/CodeGenAlloca.hpp"
#include "codegen/support/CodeGenPanic.hpp"
#include "core/ast/ExprAST.hpp"
#include "core/ast/TypeAST.hpp"

#include <llvm/IR/Type.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

/// @brief Lower the arena expression and get a pointer to the Arena struct.
///
/// According to the grammar, Arena is an "Owned, scope-confined" type with
/// no copy operation. It must be accessed through `&Arena` references.
///
/// @param expr The arena access expression.
/// @param ctx The code generation context.
/// @return Pointer to the Arena struct (i8* for runtime functions), or nullptr.
inline llvm::Value* getArenaPointer(ArenaAccessExprAST* expr, CodeGenContext& ctx) {
    if (!expr) return nullptr;

    llvm::Value* arenaExpr = lowerExpression(expr->arenaExpr, ctx);
    if (!arenaExpr) return nullptr;

    llvm::Type* i8Ptr = llvm::PointerType::get(ctx.llvmCtx, 0);
    llvm::StructType* arenaType = ctx.getArenaType();

    // ─── Case 1: arenaExpr is a pointer (should be &Arena) ──────────────────
    if (arenaExpr->getType()->isPointerTy()) {
        // Cast to i8* for runtime functions (all runtime functions take i8*)
        return ctx.builder.CreatePointerCast(arenaExpr, i8Ptr, "arena_ptr");
    }

    // ─── Case 2: arenaExpr is an Arena struct value ─────────────────────────
    // This is a compile error per the grammar: Arena cannot be passed by value.
    if (arenaExpr->getType()->isStructTy()) {
        ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->arenaExpr->loc,
                                "Arena cannot be passed by value. Use &Arena instead.\n"
                                "  Example: const buildGraph (a &Arena) = { ... }");
        return nullptr;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, expr->arenaExpr->loc,
                            "arena expression must be &Arena, got ",
                            arenaExpr->getType()->getStructName().str());
    return nullptr;
}

/// @brief Build an ArenaDescriptor from an Arena pointer.
///
/// ArenaDescriptor is { i8* base, i64 size } - the FFI-visible view of an Arena.
///
/// @param arenaPtr Pointer to the Arena struct (i8*).
/// @param ctx The code generation context.
/// @return The ArenaDescriptor value.
inline llvm::Value* buildArenaDescriptor(llvm::Value* arenaPtr, CodeGenContext& ctx) {
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);
    llvm::StructType* arenaType = ctx.getArenaType();
    llvm::StructType* descType = ctx.getArenaDescriptorType();

    // Cast i8* to Arena* for GEP
    llvm::Value* typedArenaPtr = ctx.builder.CreatePointerCast(
        arenaPtr,
        llvm::PointerType::get(arenaType, 0),
        "typed_arena_ptr"
    );

    // Load base (field 0)
    llvm::Value* basePtr = ctx.builder.CreateStructGEP(arenaType, typedArenaPtr, 0, "desc_base_ptr");
    llvm::Value* base = ctx.builder.CreateLoad(
        llvm::PointerType::get(ctx.llvmCtx, 0),
        basePtr,
        "desc_base"
    );

    // Load size (field 1)
    llvm::Value* sizePtr = ctx.builder.CreateStructGEP(arenaType, typedArenaPtr, 1, "desc_size_ptr");
    llvm::Value* size = ctx.builder.CreateLoad(i64, sizePtr, "desc_size");

    // Build ArenaDescriptor { base: i8*, size: i64 }
    llvm::Value* desc = llvm::UndefValue::get(descType);
    desc = ctx.builder.CreateInsertValue(desc, base, 0);
    desc = ctx.builder.CreateInsertValue(desc, size, 1);

    return desc;
}

/// @brief Get the size and alignment of a type for arena allocation.
///
/// @param type The Lucid type.
/// @param ctx The code generation context.
/// @return Pair of (size, alignment) in bytes.
inline std::pair<uint64_t, uint64_t> getElementSizeAndAlignment(TypeAST* type, CodeGenContext& ctx) {
    llvm::Type* llvmType = getType(ctx, type);
    if (!llvmType) return {1, 16};

    llvm::DataLayout dl(ctx.module);
    uint64_t size = dl.getTypeAllocSize(llvmType);
    if (size == 0) size = 1;

    uint64_t alignment = dl.getPrefTypeAlignment(llvmType);
    if (alignment == 0) alignment = 16;  // Default alignment

    return {size, alignment};
}

} // namespace codegen