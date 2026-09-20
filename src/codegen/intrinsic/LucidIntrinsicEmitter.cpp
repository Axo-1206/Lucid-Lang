/// @file codegen/intrinsic/LucidIntrinsicEmitter.cpp
/// @brief Implementation of the Lucid-side intrinsic emitters.

#include "LucidIntrinsicEmitter.hpp"

#include "codegen/Emitter.hpp"
#include "codegen/Program.hpp"
#include "codegen/Abi.hpp"
#include "codegen/Types.hpp"
#include "codegen/support/CodeGenPanic.hpp"

#include "core/registry/IntrinsicRegistry.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>

namespace codegen {

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Emitter Helpers
// ─────────────────────────────────────────────────────────────────────────────
//
// Each intrinsic has its own `emitX` function. The dispatcher at the
// bottom of this file routes by name. The helpers all take
// `(IntrinsicCallExprAST*, Emitter&)` and return a `Val`.

/// `#sizeof(T) -> uint64`.
/// Normally folded by Sema to a constant. If it reaches the emitter,
/// evaluate the type's size via the DataLayout and emit the constant.
Val emitSizeof(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.empty()) return {};

    // The argument is a type expression. Sema stores the resolved type
    // on the arg's `resolvedType`. Actually, for `#sizeof`, Sema
    // evaluates the type at compile time and stores the result in
    // `expr->constValue`. Reaching here means the fold didn't happen;
    // emit a fallback.
    TypeAST* targetTy = expr->args[0]->resolvedType;
    if (!targetTy) return {};

    uint64_t size = emitter.program.types().sizeOf(targetTy);
    llvm::Type* resultTy = llvm::Type::getInt64Ty(
        emitter.program.llvmContext());
    llvm::Value* result = llvm::ConstantInt::get(resultTy, size);
    return Val{result, expr->resolvedType, Own::Owned};
}

/// `#alignof(T) -> uint64`.
Val emitAlignof(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.empty()) return {};
    TypeAST* targetTy = expr->args[0]->resolvedType;
    if (!targetTy) return {};

    uint64_t align = emitter.program.types().alignOf(targetTy);
    llvm::Type* resultTy = llvm::Type::getInt64Ty(
        emitter.program.llvmContext());
    llvm::Value* result = llvm::ConstantInt::get(resultTy, align);
    return Val{result, expr->resolvedType, Own::Owned};
}

/// `#memcpy(dst, src, len) -> void`.
Val emitMemcpy(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 3) return {};

    llvm::IRBuilder<>& b = emitter.program.builder();
    llvm::Value* dst = emitter.emit(expr->args[0]).v;
    llvm::Value* src = emitter.emit(expr->args[1]).v;
    llvm::Value* len = emitter.emit(expr->args[2]).v;
    if (!dst || !src || !len) return {};

    llvm::Type* i64 = llvm::Type::getInt64Ty(emitter.program.llvmContext());
    llvm::Type* i1 = llvm::Type::getInt1Ty(emitter.program.llvmContext());

    // llvm.memcpy(dst, src, len, isvolatile)
    llvm::Function* memcpyFn = llvm::Intrinsic::getDeclaration(
        &emitter.program.module(), llvm::Intrinsic::memcpy,
        {dst->getType(), src->getType(), len->getType()});
    if (!memcpyFn) return {};

    b.CreateCall(memcpyFn, {
        dst, src, len,
        llvm::ConstantInt::get(i1, 0)   // isvolatile = false
    });
    return {};  // void
}

// ... same shape for memmove, memset, toRef, toPtr, ptrOffset, ptrDiff,
// alloc, free, arena_*, tostr, ptrstr, scope_exit ...

/// `#tostr(x) -> string`.
///
/// Dispatches on the argument's type to the appropriate runtime
/// formatter. Uses the out-pointer convention from `functions.def`:
/// allocate a `lucid.String` slot, call the formatter with the slot's
/// address, load the result.
Val emitToStr(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.empty()) return {};

    Val argVal = emitter.emit(expr->args[0]);
    if (!argVal.isValid()) return {};

    ProgramState& program = emitter.program;
    llvm::IRBuilder<>& b = program.builder();
    llvm::StructType* strTy = program.types().stringType();

    // Allocate the out-slot.
    llvm::AllocaInst* outSlot = b.CreateAlloca(strTy, nullptr, "tostr_slot");

    // Choose the formatter based on the argument's type.
    //
    // For a primitive int, `__lucid_int_to_str(out, i64)`.
    // For a float, `__lucid_float_to_str(out, double)`.
    // For a bool, `__lucid_bool_to_str(out, i8)`.
    // For a char, `__lucid_char_to_str(out, i32)`.
    // For a string, the argument is already a string — return it as-is
    // (no formatting needed).
    //
    // The dispatch mirrors the old `lowerIntrinsic` dispatch but routes
    // through `Abi` and the out-pointer convention.
    TypeAST* argTy = expr->args[0]->resolvedType;
    llvm::Value* arg = argVal.v;

    if (argTy && argTy->isa<PrimitiveTypeAST>()) {
        PrimitiveKind kind =
            argTy->as<PrimitiveTypeAST>()->primitiveKind;
        switch (kind) {
            case PrimitiveKind::Bool: {
                llvm::Value* asI8 = b.CreateZExt(
                    arg, llvm::Type::getInt8Ty(program.llvmContext()));
                program.abi().BoolToStr(b, outSlot, asI8);
                break;
            }
            case PrimitiveKind::Char: {
                llvm::Value* asI32 = b.CreateZExt(
                    arg, llvm::Type::getInt32Ty(program.llvmContext()));
                program.abi().CharToStr(b, outSlot, asI32);
                break;
            }
            case PrimitiveKind::Int:
            case PrimitiveKind::Long:
            case PrimitiveKind::Int32:
            case PrimitiveKind::Int64:
            case PrimitiveKind::Byte:
            case PrimitiveKind::Short:
            case PrimitiveKind::Int8:
            case PrimitiveKind::Int16: {
                llvm::Value* asI64 = b.CreateSExt(arg,
                    llvm::Type::getInt64Ty(program.llvmContext()));
                program.abi().IntToStr(b, outSlot, asI64);
                break;
            }
            case PrimitiveKind::Uint:
            case PrimitiveKind::Ulong:
            case PrimitiveKind::Uint32:
            case PrimitiveKind::Uint64:
            case PrimitiveKind::Ubyte:
            case PrimitiveKind::Ushort:
            case PrimitiveKind::Uint8:
            case PrimitiveKind::Uint16: {
                llvm::Value* asI64 = b.CreateZExt(arg,
                    llvm::Type::getInt64Ty(program.llvmContext()));
                program.abi().UintToStr(b, outSlot, asI64);
                break;
            }
            case PrimitiveKind::Float:
            case PrimitiveKind::Double:
            case PrimitiveKind::Decimal: {
                llvm::Value* asF64 = b.CreateFPCast(arg,
                    llvm::Type::getDoubleTy(program.llvmContext()));
                program.abi().FloatToStr(b, outSlot, asF64);
                break;
            }
            case PrimitiveKind::String: {
                // Already a string; no formatting.
                return argVal;
            }
        }
    }

    llvm::Value* result = b.CreateLoad(strTy, outSlot, "tostr_result");
    return Val{result, expr->resolvedType, Own::Owned};
}

/// `#ptrstr(p) -> string`.
Val emitPtrStr(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.empty()) return {};

    Val argVal = emitter.emit(expr->args[0]);
    if (!argVal.isValid()) return {};

    ProgramState& program = emitter.program;
    llvm::IRBuilder<>& b = program.builder();
    llvm::StructType* strTy = program.types().stringType();

    llvm::AllocaInst* outSlot = b.CreateAlloca(strTy, nullptr, "ptrstr_slot");
    program.abi().PtrToHexString(b, outSlot, argVal.v);

    llvm::Value* result = b.CreateLoad(strTy, outSlot, "ptrstr_result");
    return Val{result, expr->resolvedType, Own::Owned};
}

/// `#scope_exit(f) -> void`.
///
/// No emission. Sema registered the callback with the current block
/// (`BlockStmtAST::scopeExits`); the emitter's scope-cleanup path emits
/// the call at scope exit. The intrinsic call site is a no-op.
Val emitScopeExit(IntrinsicCallExprAST* /*expr*/, Emitter& /*emitter*/) {
    return {};
}

// ... etc. ...

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Dispatcher
// ─────────────────────────────────────────────────────────────────────────────

Val emitLucidIntrinsic(IntrinsicCallExprAST* expr,
                       const IntrinsicInfo& info,
                       Emitter& emitter) {
    // Route by name. The names are interned strings; comparing them
    // against the pool's string is O(1) via pointer comparison.
    //
    // The alternative would be an enum switch, but the intrinsics are
    // identified by strings in the AST (they come from the parser as
    // identifiers after '#'). The dispatcher compares against interned
    // names.

    InternedString name = expr->intrinsicName;
    StringPool& pool = emitter.program.pool;

    // Compile-time intrinsics.
    if (name == pool.intern("sizeof"))   return emitSizeof(expr, emitter);
    if (name == pool.intern("alignof"))  return emitAlignof(expr, emitter);
    // `#typeof` and `#nameof` produce types and names, not values; they
    // are folded by Sema and shouldn't reach the emitter.

    // Memory intrinsics.
    if (name == pool.intern("memcpy"))   return emitMemcpy(expr, emitter);
    if (name == pool.intern("memmove"))  return emitMemmove(expr, emitter);
    if (name == pool.intern("memset"))   return emitMemset(expr, emitter);

    // Allocation intrinsics.
    if (name == pool.intern("alloc"))    return emitAlloc(expr, emitter);
    if (name == pool.intern("free"))     return emitFree(expr, emitter);

    // Arena intrinsics.
    if (name == pool.intern("arena_create")) return emitArenaCreate(expr, emitter);
    if (name == pool.intern("arena_alloc"))  return emitArenaAlloc(expr, emitter);
    if (name == pool.intern("arena_reset"))  return emitArenaReset(expr, emitter);
    if (name == pool.intern("arena_free"))   return emitArenaFree(expr, emitter);

    // Pointer intrinsics.
    if (name == pool.intern("toRef"))    return emitToRef(expr, emitter);
    if (name == pool.intern("toPtr"))    return emitToPtr(expr, emitter);
    if (name == pool.intern("ptrOffset")) return emitPtrOffset(expr, emitter);
    if (name == pool.intern("ptrDiff"))  return emitPtrDiff(expr, emitter);

    // Formatting intrinsics.
    if (name == pool.intern("tostr"))    return emitToStr(expr, emitter);
    if (name == pool.intern("ptrstr"))   return emitPtrStr(expr, emitter);

    // Scope intrinsics.
    if (name == pool.intern("scope_exit")) return emitScopeExit(expr, emitter);

    // Unrecognized. Sema's IntrinsicValidator should have rejected this.
    emitter.program.diagnostics.errorAt(
        DiagCode::Sem_UnknownIntrinsic, expr->loc,
        "unsupported intrinsic '#", pool.lookup(name), "'");
    return {};
}

} // namespace codegen