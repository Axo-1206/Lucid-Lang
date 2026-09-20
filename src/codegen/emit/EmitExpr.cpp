/// @file codegen/emit/EmitExpr.cpp
/// @brief Expression lowering — the `Emitter::emit(ExprAST*)` entry point
///        and its per-kind dispatch.
///
/// ─── The Ownership Tag ────────────────────────────────────────────────────
/// Every emitter returns a `Val` with an `Own` tag:
///
///   - `Owned` — a fresh value carrying a claim the receiver takes over.
///               Literals, calls, struct literals, closures, binary ops.
///
///   - `Borrowed` — an alias to a claim held elsewhere. Identifier loads,
///                  field loads, index loads.
///
/// The tag replaces `isFreshExpression`. Where the old code inspected the
/// AST to guess the tag, the new emitters know it because they produced
/// the value.

#include "Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"
#include "codegen/support/Truthiness.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emit — the public dispatch
// ─────────────────────────────────────────────────────────────────────────────

Val Emitter::emit(ExprAST* expr) {
    if (!expr) return {};

    // ─── Folded constant short-circuit ────────────────────────────────────
    // Sema folds some expressions to `ConstantValue`s. If this one was
    // folded, emit the constant directly.
    if (expr->isConst && expr->constValue.isEvaluated()) {
        if (Val folded = emitFoldedConstant(expr)) {
            return folded;
        }
        // Fall through for constants `emitFoldedConstant` doesn't handle.
    }

    switch (expr->kind) {
        case ASTKind::LiteralExpr:       return emitLiteral(expr->as<LiteralExprAST>());
        case ASTKind::IdentifierExpr:    return emitIdentifier(expr->as<IdentifierExprAST>());
        case ASTKind::ArrayLiteralExpr:  return emitArrayLiteral(expr->as<ArrayLiteralExprAST>());
        case ASTKind::StructLiteralExpr: return emitStructLiteral(expr->as<StructLiteralExprAST>());
        case ASTKind::BinaryExpr:        return emitBinary(expr->as<BinaryExprAST>());
        case ASTKind::UnaryExpr:         return emitUnary(expr->as<UnaryExprAST>());
        case ASTKind::CallExpr:          return emitCall(expr->as<CallExprAST>());
        case ASTKind::IntrinsicCallExpr: return emitIntrinsic(expr->as<IntrinsicCallExprAST>());
        case ASTKind::IndexExpr:         return emitIndex(expr->as<IndexExprAST>());
        case ASTKind::SliceExpr:         return emitSlice(expr->as<SliceExprAST>());
        case ASTKind::FieldAccessExpr:   return emitFieldAccess(expr->as<FieldAccessExprAST>());
        case ASTKind::ModuleAccessExpr:  return emitModuleAccess(expr->as<ModuleAccessExprAST>());
        case ASTKind::ArenaAccessExpr:   return emitArenaAccess(expr->as<ArenaAccessExprAST>());
        case ASTKind::NullCoalesceExpr:  return emitNullCoalesce(expr->as<NullCoalesceExprAST>());
        case ASTKind::AssignExpr:        return emitAssign(expr->as<AssignExprAST>());
        case ASTKind::PipelineExpr:      return emitPipeline(expr->as<PipelineExprAST>());
        case ASTKind::AnonFuncExpr:      return emitAnonFunc(expr->as<AnonFuncExprAST>());
        case ASTKind::IfExpr:            return emitIf(expr->as<IfExprAST>());
        case ASTKind::RangeExpr:         return emitRange(expr->as<RangeExprAST>());

        default:
            // Sema should have rejected unknown kinds. Reaching here is
            // a compiler bug.
            return {};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emitLiteral — a scalar constant
// ─────────────────────────────────────────────────────────────────────────────
//
// A literal has no resource content (except a string, which produces a
// fresh `Owned` string value). Scalars are `Owned` in the sense that the
// receiver takes over the value's claim — for scalars, `intoOwned` is a
// no-op, so the tag doesn't matter in practice.

Val Emitter::emitLiteral(LiteralExprAST* expr) {
    llvm::IRBuilder<>& b = program.builder();
    llvm::LLVMContext& ctx = program.llvmContext();

    llvm::Type* ty = program.types().get(expr->resolvedType);
    if (!ty) return {};

    llvm::Value* result = nullptr;

    switch (expr->kind) {
        case LiteralKind::True:
            result = llvm::ConstantInt::get(ty, 1);
            break;
        case LiteralKind::False:
            result = llvm::ConstantInt::get(ty, 0);
            break;

        case LiteralKind::Int:
        case LiteralKind::Hex:
        case LiteralKind::Binary: {
            std::string valStr = program.pool.lookup(expr->value);
            int64_t val = 0;
            try {
                if (expr->kind == LiteralKind::Hex) {
                    val = std::stoll(valStr, nullptr, 16);
                } else if (expr->kind == LiteralKind::Binary) {
                    val = std::stoll(valStr, nullptr, 2);
                } else {
                    val = std::stoll(valStr, nullptr, 10);
                }
            } catch (const std::exception&) {
                program.diagnostics.errorAt(DiagCode::Lex_InvalidNumberLiteral,
                                             expr->loc,
                                             "invalid integer literal: ", valStr);
                return {};
            }
            result = llvm::ConstantInt::get(ty, val);
            break;
        }

        case LiteralKind::Float: {
            std::string valStr = program.pool.lookup(expr->value);
            double val = 0.0;
            try {
                val = std::stod(valStr);
            } catch (const std::exception&) {
                program.diagnostics.errorAt(DiagCode::Lex_InvalidNumberLiteral,
                                             expr->loc,
                                             "invalid float literal: ", valStr);
                return {};
            }
            result = llvm::ConstantFP::get(ty, val);
            break;
        }

        case LiteralKind::String:
        case LiteralKind::RawString: {
            // A string literal lowers to a `lucid.String` value with a
            // private global for the bytes and `cap == 0` to mark it
            // static. The `Owned` tag means the receiver takes over the
            // claim — for a static string, "the claim" is meaningless
            // (the data isn't heap-allocated), so `intoOwned`'s
            // `OwnedBuffer` path checks `cap == 0` and skips the deep
            // copy. The result is `Owned` with a shared static buffer.
            std::string valStr = program.pool.lookup(expr->value);
            result = program.types().stringLiteral(valStr, b);
            break;
        }

        case LiteralKind::Char: {
            std::string valStr = program.pool.lookup(expr->value);
            if (valStr.empty()) {
                result = llvm::ConstantInt::get(ty, 0);
            } else {
                result = llvm::ConstantInt::get(ty, valStr[0]);
            }
            break;
        }

        case LiteralKind::Nil:
        case LiteralKind::Err:
            // A nil/err literal in a tagged-slot context is a tagged
            // slot with the sentinel tag. The emitter's caller
            // (target-typed emit) builds the tagged slot; here, we emit
            // the sentinel as a zero-valued slot.
            result = llvm::Constant::getNullValue(ty);
            break;

        default:
            return {};
    }

    return Val{result, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIdentifier — a load from a binding
// ─────────────────────────────────────────────────────────────────────────────
//
// The result is `Borrowed`: the binding still holds the claim, and the
// loaded value is an alias. When the caller wants to store the value,
// `store` calls `intoOwned` to acquire a fresh claim.
//
// Exception: a `cls`-shaped `FuncDeclAST` binding holds the fat pointer
// by value, not by pointer. The emitter returns that value directly,
// still `Borrowed` (the binding's value map holds the fat pointer, and
// the value returned is a copy of the fat pointer — copying the fat
// pointer doesn't retain the env, so the claim is still the binding's).

Val Emitter::emitIdentifier(IdentifierExprAST* expr) {
    assert(expr && "emitIdentifier() with null expression");

    // ─── Special case: `_` discard placeholder ────────────────────────────
    if (program.pool.lookupView(expr->name) == "_") {
        // Using `_` as a value is a Sema error. Reaching here is a bug.
        return {};
    }

    ValueDeclAST* decl = expr->resolvedDecl;
    if (!decl) return {};

    llvm::IRBuilder<>& b = program.builder();

    // ─── Function reference ───────────────────────────────────────────────
    if (decl->isa<FuncDeclAST>()) {
        FuncDeclAST* fn = decl->as<FuncDeclAST>();

        // `fn`-shaped: bare function pointer.
        FuncShape shape = fn->funcType ? fn->funcType->shape : FuncShape::Fn;
        if (shape == FuncShape::Fn) {
            llvm::Function* llvmFn = program.lookupFunction(fn);
            if (!llvmFn) return {};
            return Val{llvmFn, expr->resolvedType, Own::Borrowed};
        }

        // `cls`-shaped: fat pointer stored by value.
        llvm::Value* closureVal = func().lookupValue(fn);
        if (!closureVal) return {};
        return Val{closureVal, expr->resolvedType, Own::Borrowed};
    }

    // ─── Enum variant ─────────────────────────────────────────────────────
    if (decl->isa<EnumVariantAST>()) {
        EnumVariantAST* variant = decl->as<EnumVariantAST>();
        llvm::Type* enumTy = program.types().get(expr->resolvedType);
        if (!enumTy || !enumTy->isIntegerTy()) return {};

        llvm::Value* c = llvm::ConstantInt::get(
            enumTy, static_cast<uint64_t>(variant->value), /*isSigned=*/true);
        return Val{c, expr->resolvedType, Own::Owned};
    }

    // ─── Local binding: load from the place ───────────────────────────────
    // The binding's storage lives in the value map. It's a pointer to
    // the storage (alloca or spill slot); the emitter loads the value.
    llvm::Value* binding = func().lookupValue(decl);
    if (!binding) return {};

    // A non-pointer binding is an SSA value (e.g. a closure's fat pointer
    // held by value). Return it directly.
    if (!binding->getType()->isPointerTy()) {
        return Val{binding, decl->type, Own::Borrowed};
    }

    llvm::Type* valueTy = program.types().get(decl->type);
    if (!valueTy) return {};

    llvm::Value* loaded = b.CreateLoad(
        valueTy, binding, "load_" + program.pool.lookup(expr->name));

    return Val{loaded, decl->type, Own::Borrowed};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitCall — a function call
// ─────────────────────────────────────────────────────────────────────────────
//
// The result is `Owned`: the callee's return-value rules determine the
// claim, and per Rule 3 the callee transfers the claim to the caller. For
// a `void` return, the result is an invalid `Val` (no value, no claim).
//
// The ownership work in `emitCall`:
//   1. For each argument, `intoOwned` before passing — a `Borrowed`
//      argument becomes a fresh `Owned` value that the callee's
//      parameter binding takes over.
//   2. This is the retain-on-argument-pass rule (Rule 3).

Val Emitter::emitCall(CallExprAST* expr) {
    assert(expr && "emitCall() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Emit the callee ──────────────────────────────────────────────────
    Val calleeVal = emit(expr->callee);
    if (!calleeVal.isValid()) return {};

    // ─── Function type ────────────────────────────────────────────────────
    FuncTypeAST* calleeFuncTy = expr->callee->resolvedType
        ? (expr->callee->resolvedType->isa<FuncTypeAST>()
              ? expr->callee->resolvedType->as<FuncTypeAST>()
              : nullptr)
        : nullptr;
    if (!calleeFuncTy) return {};

    llvm::FunctionType* fnTy = program.types().functionType(
        calleeFuncTy, /*isClosure=*/false);
    if (!fnTy) return {};

    // ─── Emit arguments ───────────────────────────────────────────────────
    // Argument passing is where ownership gets interesting. Each argument
    // is:
    //   1. Emitted (may be Owned or Borrowed).
    //   2. Coerced (fn → cls, if the parameter is cls).
    //   3. Acquired as Owned via intoOwned — this is the retain-on-copy
    //      rule for closure arguments.
    //
    // For non-resource arguments, intoOwned is a no-op; the extra call
    // is unmeasurable.
    std::vector<llvm::Value*> args;
    args.reserve(expr->args.size());

    for (size_t i = 0; i < expr->args.size(); ++i) {
        ExprAST* argExpr = expr->args[i];
        Val argVal = emit(argExpr);
        if (!argVal.isValid()) return {};

        // Parameter type for this argument.
        TypeAST* paramTy = (i < calleeFuncTy->params.size())
            ? calleeFuncTy->params[i]->type
            : nullptr;

        // fn → cls coercion.
        if (paramTy) {
            argVal.v = maybeCoerceFnToCls(argVal.v, argVal.ty, paramTy,
                                           *this);
            // The maybeCoerceFnToCls transition is handled by the ownership
            // module; it may return a wrapped value.
        }

        // Acquire a fresh claim.
        Val owned = program.ownership().intoOwned(argVal, b);
        args.push_back(owned.v);
    }

    // ─── Emit the call ────────────────────────────────────────────────────
    llvm::Value* result = emitCallableCall(
        calleeVal.v, args, fnTy, calleeFuncTy->shape, b, "call");

    if (!result) return {};

    // ─── Return type ──────────────────────────────────────────────────────
    TypeAST* returnTy = calleeFuncTy->returnType;
    if (!returnTy) {
        // Void return. No value, no claim.
        return {};
    }

    return Val{result, returnTy, Own::Owned};
}

// ... other emitters follow the same shape ...

} // namespace codegen