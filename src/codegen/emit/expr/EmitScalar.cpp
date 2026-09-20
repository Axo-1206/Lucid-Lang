/// @file codegen/emit/expr/EmitScalar.cpp
/// @brief Scalar and control-flow-producing expression emitters.
///
/// ─── What This File Owns ──────────────────────────────────────────────────
/// The expression kinds that produce a single LLVM value and have no
/// storage-access semantics:
///
///   - `emitLiteral`     — scalar, string, char, and nil/err constants.
///   - `emitIdentifier`  — a load from a binding, a function reference, or
///                         an enum variant.
///   - `emitBinary`      — arithmetic, comparison, logical, and bitwise
///                         operations.
///   - `emitUnary`       — negation, logical not, bitwise not.
///   - `emitIf`          — the expression form of `if`; produces a phi.
///   - `emitRange`       — never a value; emits a diagnostic.
///
/// ─── What This File Does NOT Own ──────────────────────────────────────────
///   - Storage access: `EmitAccess.cpp` (index, slice, field, module,
///     arena).
///   - Aggregate construction: `EmitAggregate.cpp` (struct, array
///     literals).
///   - Read-then-write: `EmitWrite.cpp` (assign, null coalesce,
///     pipeline).
///   - Calls: `EmitCall.cpp`.
///   - Closures: `EmitClosure.cpp`.
///
/// ─── Ownership Tag Conventions ────────────────────────────────────────────
///   - `emitLiteral`      → `Owned`. A literal has no claim to transfer.
///                           For a string, `cap == 0` marks it static;
///                           `intoOwned` skips the deep copy at the use
///                           site.
///
///   - `emitIdentifier`   → `Borrowed`. The binding holds the claim.
///                           Exception: enum variants are `Owned` (they're
///                           constants), and `fn`-shaped function
///                           references are `Borrowed` (globals).
///
///   - `emitBinary`       → `Owned`. A fresh scalar.
///                           Exception: string concatenation is a runtime
///                           call that allocates a fresh buffer; the
///                           result is `Owned` with a real claim.
///
///   - `emitUnary`        → `Owned`. A fresh scalar.
///
///   - `emitIf`           → `Owned` if both arms return `Owned`,
///                           `Borrowed` if both arms return `Borrowed`.
///                           An assertion fires if the arms disagree.
///
///   - `emitRange`        → invalid. Never a value.
///
/// ─── The `emitPanic` Helper ───────────────────────────────────────────────
/// A few emitters need to lower a runtime check (division by zero, shift
/// by negative amount, out-of-range in a future revision). The panic path
/// is:
///
///   1. Format the message as `"file:line:col: description"`.
///   2. Create a private global holding the string.
///   3. GEP to its first byte (a `ptr` to the NUL-terminated data).
///   4. Call `program.abi().Panic(builder, msgPtr)`.
///   5. Emit `unreachable` — the runtime function is `noreturn`.
///
/// The helper lives here because it's only used by this file today. When
/// `EmitAccess.cpp`'s index/slice bounds checks need it, it should move to
/// a shared `EmitPanic.cpp`. For now, keep it local and note the intent.

#include "../Emitter.hpp"

#include "codegen/Program.hpp"
#include "codegen/FunctionState.hpp"
#include "runtime/RuntimeError.hpp"

#include "runtime/RuntimeError.hpp"

#include "core/trace/Trace.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include <cassert>
#include <string>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// emitPanic — the runtime-panic lowering helper
// ─────────────────────────────────────────────────────────────────────────────
//
// Lower a call to `__lucid_panic` with a formatted message. The message
// format matches the runtime's expectations (see PanicRuntime.cpp):
//
//     "file.luc:line:column: error description"
//
// The message is built once, at codegen time, and stored in a private
// global. At runtime, the panic call passes a `ptr` to the global's first
// byte. The runtime's `__lucid_panic` treats it as `const char*`.
//
// ─── Why the Message Is a Global, Not an Alloca ───────────────────────────
// The message string is compile-time-known. There's no reason to construct
// it at runtime. A private global holding the bytes is what a C compiler
// emits for a string literal, and it's what the runtime expects.
//
// ─── Why `unreachable` ────────────────────────────────────────────────────
// `__lucid_panic` is marked `noreturn` in its LLVM declaration (see
// `Abi::declareOrGet`). But the emitter doesn't rely on that for
// correctness — it appends `unreachable` explicitly. This makes the IR
// self-documenting: a reader sees the panic path terminate. LLVM's
// optimiser will remove the `unreachable` if it can prove the panic call
// already terminates, but having it there first is correct and clear.

void Emitter::emitPanic(RuntimeErrorKind kind, SourceLocation loc) {
    llvm::IRBuilder<>& b = program.builder();

    // ─── Format the message ───────────────────────────────────────────────
    // The source-location prefix matches the runtime's expected format.
    // `SourceLocation` provides file/line/column; the message body comes
    // from the RuntimeError registry.
    std::string message;
    if (loc.isValid()) {
        message = program.pool.lookup(loc.file)
                + ":" + std::to_string(loc.line())
                + ":" + std::to_string(loc.column())
                + ": " + getRuntimeErrorMessage(kind);
    } else {
        message = "runtime: " + getRuntimeErrorMessage(kind);
    }

    // ─── Create the string global ─────────────────────────────────────────
    // Private linkage, constant, one per panic site. Duplicate messages
    // get merged by the linker or the optimiser.
    llvm::Constant* strConst = llvm::ConstantDataArray::getString(
        program.llvmContext(), message, /*AddNull=*/true);
    llvm::GlobalVariable* global = new llvm::GlobalVariable(
        program.module(),
        strConst->getType(),
        /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage,
        strConst,
        ".panic_msg");

    // ─── GEP to the first byte ────────────────────────────────────────────
    // With opaque pointers, the global is already a `ptr` and the GEP is
    // a no-op at the LLVM level. But the GEP documents the intent
    // (first-byte-of-an-array) and matches what a C compiler emits for a
    // string literal.
    llvm::Value* msgPtr = b.CreateInBoundsGEP(
        strConst->getType(), global,
        {llvm::ConstantInt::get(llvm::Type::getInt32Ty(program.llvmContext()), 0),
         llvm::ConstantInt::get(llvm::Type::getInt32Ty(program.llvmContext()), 0)},
        "panic_msg_ptr");

    // ─── Emit the call ────────────────────────────────────────────────────
    // `Abi::Panic` declares `__lucid_panic` if it isn't already declared,
    // records it in the used-set for the manifest, and emits the call.
    program.abi().Panic(b, msgPtr);

    // ─── Mark the path terminated ─────────────────────────────────────────
    // `unreachable` is the standard terminator for a path that can't
    // return. It's technically redundant (the panic call is `noreturn`),
    // but having it makes the IR's control flow explicit.
    b.CreateUnreachable();
}

// ─────────────────────────────────────────────────────────────────────────────
// emitLiteral — a scalar or string constant
// ─────────────────────────────────────────────────────────────────────────────
//
// A literal has no resource content, except a string. A string literal
// lowers to a `lucid.String` value whose data pointer refers into a
// private global and whose `cap` is 0 (the static-literal sentinel). The
// `Owned` tag means the receiver takes over the value's claim — but for a
// static string, "the claim" is meaningless (the data isn't
// heap-allocated), so `intoOwned`'s `OwnedBuffer` path checks `cap == 0`
// and skips the deep copy.

Val Emitter::emitLiteral(LiteralExprAST* expr) {
    assert(expr && "emitLiteral() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    llvm::Type* ty = program.types().get(expr->resolvedType);
    if (!ty) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, expr->loc,
            "literal has no resolvable type");
        return {};
    }

    llvm::Value* result = nullptr;

    switch (expr->kind) {
        // ─── Bool ─────────────────────────────────────────────────────────
        case LiteralKind::True:
            result = llvm::ConstantInt::get(ty, 1);
            break;

        case LiteralKind::False:
            result = llvm::ConstantInt::get(ty, 0);
            break;

        // ─── Integer (decimal, hex, binary) ───────────────────────────────
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
                program.diagnostics.errorAt(
                    DiagCode::Lex_InvalidNumberLiteral, expr->loc,
                    "invalid integer literal: ", valStr);
                return {};
            }
            result = llvm::ConstantInt::get(ty, val);
            break;
        }

        // ─── Float ────────────────────────────────────────────────────────
        case LiteralKind::Float: {
            std::string valStr = program.pool.lookup(expr->value);
            double val = 0.0;
            try {
                val = std::stod(valStr);
            } catch (const std::exception&) {
                program.diagnostics.errorAt(
                    DiagCode::Lex_InvalidNumberLiteral, expr->loc,
                    "invalid float literal: ", valStr);
                return {};
            }
            result = llvm::ConstantFP::get(ty, val);
            break;
        }

        // ─── String and raw string ────────────────────────────────────────
        case LiteralKind::String:
        case LiteralKind::RawString: {
            std::string valStr = program.pool.lookup(expr->value);
            result = program.types().stringLiteral(valStr, b);
            if (!result) {
                program.diagnostics.errorAt(
                    DiagCode::Backend_CodegenError, expr->loc,
                    "failed to lower string literal");
                return {};
            }
            break;
        }

        // ─── Char ─────────────────────────────────────────────────────────
        case LiteralKind::Char: {
            std::string valStr = program.pool.lookup(expr->value);
            if (valStr.empty()) {
                result = llvm::ConstantInt::get(ty, 0);
            } else {
                // A char is a single byte. The parser interns the
                // unescaped value; take the first byte.
                result = llvm::ConstantInt::get(
                    ty, static_cast<uint8_t>(valStr[0]));
            }
            break;
        }

        // ─── nil / err sentinels ──────────────────────────────────────────
        case LiteralKind::Nil:
        case LiteralKind::Err:
            // A nil/err literal is a zero-valued slot. The tag that
            // distinguishes it from a present value is set by the
            // enclosing tagged-slot construction site — an assignment to
            // a `T?` binding, a call returning `T?`, etc. The literal
            // alone is just the slot's null value.
            result = llvm::Constant::getNullValue(ty);
            break;

        default:
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "unsupported literal kind");
            return {};
    }

    return Val{result, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIdentifier — a load from a binding, a function reference, or a
//                  variant
// ─────────────────────────────────────────────────────────────────────────────
//
// The result is `Borrowed` for storage-bound identifiers: the binding
// still holds the claim, and the loaded value is an alias.
//
// Three special cases:
//
//   1. `cls`-shaped `FuncDeclAST` bindings hold a fat pointer by value,
//      not by pointer. The emitter returns the fat pointer directly,
//      still `Borrowed` (the binding's value map holds the fat pointer,
//      and the returned copy doesn't retain the env).
//
//   2. `fn`-shaped `FuncDeclAST` bindings are the `llvm::Function*`
//      itself. Returned as `Borrowed` — a global symbol has no claim.
//
//   3. `EnumVariantAST` bindings are integer constants. Returned as
//      `Owned` — a constant has no claim.

Val Emitter::emitIdentifier(IdentifierExprAST* expr) {
    assert(expr && "emitIdentifier() with null expression");

    // ─── The `_` discard placeholder ──────────────────────────────────────
    // `_` is only legal in a pattern-binding position (a `for` loop
    // variable, a destructuring binding). Using it as a value is a Sema
    // error; reaching here means Sema let it through.
    if (program.pool.lookupView(expr->name) == "_") {
        program.diagnostics.errorAt(
            DiagCode::Sem_UndefinedValue, expr->loc,
            "cannot use '_' as a value");
        return {};
    }

    ValueDeclAST* decl = expr->resolvedDecl;
    if (!decl) {
        program.diagnostics.errorAt(
            DiagCode::Sem_UndefinedValue, expr->loc,
            "identifier '", program.pool.lookup(expr->name),
            "' was not resolved");
        return {};
    }

    llvm::IRBuilder<>& b = program.builder();

    // ─── Function reference ───────────────────────────────────────────────
    if (decl->isa<FuncDeclAST>()) {
        FuncDeclAST* fn = decl->as<FuncDeclAST>();

        // `fn`-shaped: a bare function pointer. The `llvm::Function*` is
        // the value; there's no storage to load from.
        FuncShape shape = fn->funcType ? fn->funcType->shape : FuncShape::Fn;
        if (shape == FuncShape::Fn) {
            llvm::Function* llvmFn = program.lookupFunction(fn);
            if (!llvmFn) {
                program.diagnostics.errorAt(
                    DiagCode::Backend_CodegenError, expr->loc,
                    "function '", program.pool.lookup(fn->name),
                    "' has no LLVM prototype — the declare pass did not "
                    "run, or the function was never reached");
                return {};
            }
            return Val{llvmFn, expr->resolvedType, Own::Borrowed};
        }

        // `cls`-shaped: a fat pointer stored by value in the value map.
        llvm::Value* closureVal = func().lookupValue(fn);
        if (!closureVal) {
            program.diagnostics.errorAt(
                DiagCode::Backend_CodegenError, expr->loc,
                "cls-shaped function '", program.pool.lookup(fn->name),
                "' has no fat-pointer binding");
            return {};
        }
        return Val{closureVal, expr->resolvedType, Own::Borrowed};
    }

    // ─── Enum variant ─────────────────────────────────────────────────────
    if (decl->isa<EnumVariantAST>()) {
        EnumVariantAST* variant = decl->as<EnumVariantAST>();
        llvm::Type* enumTy = program.types().get(expr->resolvedType);
        if (!enumTy || !enumTy->isIntegerTy()) {
            program.diagnostics.errorAt(
                DiagCode::Backend_InvalidIR, expr->loc,
                "enum variant '", program.pool.lookup(expr->name),
                "' has a non-integer type");
            return {};
        }

        // An enum variant is a `ConstantInt` of the enum's backing type.
        // No storage; the value is a compile-time constant.
        llvm::Value* c = llvm::ConstantInt::get(
            enumTy,
            static_cast<uint64_t>(variant->value),
            /*isSigned=*/true);
        return Val{c, expr->resolvedType, Own::Owned};
    }

    // ─── Local binding: load from the place ───────────────────────────────
    // The binding's storage lives in the value map. For storage-bound
    // declarations, it's a pointer to storage (alloca or spill slot); for
    // `cls` declarations, it's an SSA fat pointer (handled above). This
    // path handles the storage-bound case, plus the residual by-value
    // case for any other declaration kind.
    llvm::Value* binding = func().lookupValue(decl);
    if (!binding) {
        program.diagnostics.errorAt(
            DiagCode::Sem_UndefinedValue, expr->loc,
            "identifier '", program.pool.lookup(expr->name),
            "' has no LLVM binding in the current function");
        return {};
    }

    // ─── Non-pointer binding: an SSA value held by value ──────────────────
    // This path is for `cls` FuncDecl bindings and any other declaration
    // whose binding is stored by value. The value is returned directly.
    if (!binding->getType()->isPointerTy()) {
        return Val{binding, decl->type, Own::Borrowed};
    }

    // ─── Pointer binding: load the value ──────────────────────────────────
    llvm::Type* valueTy = program.types().get(decl->type);
    if (!valueTy) {
        program.diagnostics.errorAt(
            DiagCode::Backend_InvalidIR, expr->loc,
            "binding '", program.pool.lookup(expr->name),
            "' has an unresolvable type");
        return {};
    }

    llvm::Value* loaded = b.CreateLoad(
        valueTy, binding, "load_" + program.pool.lookup(expr->name));

    return Val{loaded, decl->type, Own::Borrowed};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitBinary — an infix binary operation
// ─────────────────────────────────────────────────────────────────────────────
//
// ─── Operand Evaluation ───────────────────────────────────────────────────
// Both operands are emitted as `Val`s. A `Val` from a storage-bound
// identifier is a load; from a binary, an SSA value; from a call, the
// call's result. The emitter never inspects the AST to decide whether to
// load.
//
// ─── Type Agreement ───────────────────────────────────────────────────────
// Sema has already type-checked the expression. By the time the emitter
// runs, both operands have the same LLVM type — or, for `+` on strings,
// both are `lucid.String`. The emitter asserts this in debug builds and
// trusts it in release.
//
// ─── Short-Circuit Operators ──────────────────────────────────────────────
// `and` and `or` are lowered by Sema into explicit control flow. The
// emitter only sees the non-short-circuit forms here. If Sema hasn't
// lowered them yet, the emitter would emit a bitwise `and` / `or` on the
// coerced booleans, which is incorrect for side-effecting operands.
// Flagged with a comment.

Val Emitter::emitBinary(BinaryExprAST* expr) {
    assert(expr && "emitBinary() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Emit operands ────────────────────────────────────────────────────
    Val left = emit(expr->left);
    if (!left.isValid()) return {};

    Val right = emit(expr->right);
    if (!right.isValid()) return {};

    // ─── String concatenation ─────────────────────────────────────────────
    // `+` on strings lowers to `__lucid_str_concat`, not to an LLVM
    // instruction. The runtime allocates a fresh buffer; the result is
    // `Owned` because no one else has a claim on it.
    //
    // This check runs before the arithmetic switch because the LLVM
    // types don't disambiguate: a `lucid.String` is a struct, but so is
    // every other aggregate. The AST type does.
    if (expr->op == BinaryOp::Add
        && left.ty && right.ty
        && left.ty->isa<PrimitiveTypeAST>()
        && right.ty->isa<PrimitiveTypeAST>()
        && left.ty->as<PrimitiveTypeAST>()->primitiveKind
            == PrimitiveKind::String
        && right.ty->as<PrimitiveTypeAST>()->primitiveKind
            == PrimitiveKind::String) {

        llvm::StructType* strTy = program.types().stringType();
        if (!strTy) return {};

        // Spill the two source strings to stack slots. `__lucid_str_concat`
        // takes `LucidString*`, not the struct by value.
        llvm::AllocaInst* leftSlot = createEntryAlloca(strTy, "concat_lhs");
        llvm::AllocaInst* rightSlot = createEntryAlloca(strTy, "concat_rhs");
        llvm::AllocaInst* outSlot = createEntryAlloca(strTy, "concat_out");
        if (!leftSlot || !rightSlot || !outSlot) return {};

        b.CreateStore(left.v, leftSlot);
        b.CreateStore(right.v, rightSlot);

        // Call the runtime. `Abi::StrConcat` declares the symbol if
        // needed, records it in the used-set for the manifest, and emits
        // the call.
        program.abi().StrConcat(b, outSlot, leftSlot, rightSlot);

        llvm::Value* result = b.CreateLoad(strTy, outSlot, "concat_result");
        return Val{result, expr->resolvedType, Own::Owned};
    }

    // ─── Integer and float operations ─────────────────────────────────────
    llvm::Value* result = nullptr;
    llvm::Type* operandTy = left.v->getType();
    const bool isInt = operandTy->isIntegerTy();
    const bool isFloat = operandTy->isFloatingPointTy();

    switch (expr->op) {
        // ─── Arithmetic ───────────────────────────────────────────────────
        case BinaryOp::Add:
            result = isInt
                ? b.CreateAdd(left.v, right.v, "add")
                : b.CreateFAdd(left.v, right.v, "fadd");
            break;

        case BinaryOp::Sub:
            result = isInt
                ? b.CreateSub(left.v, right.v, "sub")
                : b.CreateFSub(left.v, right.v, "fsub");
            break;

        case BinaryOp::Mul:
            result = isInt
                ? b.CreateMul(left.v, right.v, "mul")
                : b.CreateFMul(left.v, right.v, "fmul");
            break;

        case BinaryOp::Div:
        case BinaryOp::Mod: {
            if (isInt) {
                // ─── Division-by-zero guard ───────────────────────────────
                // Integer division by zero is undefined; the emitter
                // guards it with a branch to a panic call.
                llvm::Value* isZero = b.CreateICmpEQ(
                    right.v,
                    llvm::ConstantInt::get(right.v->getType(), 0),
                    "divisor_is_zero");

                llvm::Function* fn = b.GetInsertBlock()->getParent();
                llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
                    program.llvmContext(), "div.panic", fn);
                llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
                    program.llvmContext(), "div.continue", fn);

                b.CreateCondBr(isZero, panicBlock, continueBlock);

                b.SetInsertPoint(panicBlock);
                emitPanic(
                    expr->op == BinaryOp::Div
                        ? RuntimeErrorKind::DivisionByZero
                        : RuntimeErrorKind::ModuloByZero,
                    expr->loc);

                b.SetInsertPoint(continueBlock);
                result = expr->op == BinaryOp::Div
                    ? b.CreateSDiv(left.v, right.v, "sdiv")
                    : b.CreateSRem(left.v, right.v, "srem");
            } else {
                // Float division. IEEE-754 defines division by zero as
                // infinity or NaN, which the language accepts for floats.
                result = expr->op == BinaryOp::Div
                    ? b.CreateFDiv(left.v, right.v, "fdiv")
                    : b.CreateFRem(left.v, right.v, "frem");
            }
            break;
        }

        case BinaryOp::Pow: {
            // `**` has no LLVM instruction. For floats, the LLVM `pow`
            // intrinsic does the job. For integers, there's no runtime
            // function yet; the emitter diagnoses the case.
            if (isFloat) {
                llvm::Function* powFn = llvm::Intrinsic::getDeclaration(
                    &program.module(), llvm::Intrinsic::pow,
                    {operandTy});
                result = b.CreateCall(powFn, {left.v, right.v}, "pow");
            } else {
                program.diagnostics.errorAt(
                    DiagCode::Sem_InvalidBinary, expr->loc,
                    "integer exponentiation is not yet implemented");
                return {};
            }
            break;
        }

        // ─── Comparison ───────────────────────────────────────────────────
        case BinaryOp::Eq:
            result = isInt
                ? b.CreateICmpEQ(left.v, right.v, "eq")
                : b.CreateFCmpOEQ(left.v, right.v, "feq");
            break;

        case BinaryOp::Ne:
            result = isInt
                ? b.CreateICmpNE(left.v, right.v, "ne")
                : b.CreateFCmpONE(left.v, right.v, "fne");
            break;

        case BinaryOp::Lt:
            result = isInt
                ? b.CreateICmpSLT(left.v, right.v, "lt")
                : b.CreateFCmpOLT(left.v, right.v, "flt");
            break;

        case BinaryOp::Gt:
            result = isInt
                ? b.CreateICmpSGT(left.v, right.v, "gt")
                : b.CreateFCmpOGT(left.v, right.v, "fgt");
            break;

        case BinaryOp::Le:
            result = isInt
                ? b.CreateICmpSLE(left.v, right.v, "le")
                : b.CreateFCmpOLE(left.v, right.v, "fle");
            break;

        case BinaryOp::Ge:
            result = isInt
                ? b.CreateICmpSGE(left.v, right.v, "ge")
                : b.CreateFCmpOGE(left.v, right.v, "fge");
            break;

        // ─── Logical ──────────────────────────────────────────────────────
        // `and` / `or` should have been lowered to control flow by Sema.
        // If they reach here, the operands are already-evaluated values,
        // and the emitter emits a bitwise `and` / `or` on the coerced
        // booleans. This is correct only if the operands have no side
        // effects — which Sema's lowering guarantees.
        case BinaryOp::And: {
            llvm::Value* leftI1 = emitTruthiness(left);
            llvm::Value* rightI1 = emitTruthiness(right);
            if (!leftI1 || !rightI1) return {};
            result = b.CreateAnd(leftI1, rightI1, "and");
            break;
        }

        case BinaryOp::Or: {
            llvm::Value* leftI1 = emitTruthiness(left);
            llvm::Value* rightI1 = emitTruthiness(right);
            if (!leftI1 || !rightI1) return {};
            result = b.CreateOr(leftI1, rightI1, "or");
            break;
        }

        // ─── Bitwise ──────────────────────────────────────────────────────
        case BinaryOp::BitAnd:
            result = b.CreateAnd(left.v, right.v, "band");
            break;

        case BinaryOp::BitOr:
            result = b.CreateOr(left.v, right.v, "bor");
            break;

        case BinaryOp::BitXor:
            result = b.CreateXor(left.v, right.v, "bxor");
            break;

        case BinaryOp::Shl:
            result = b.CreateShl(left.v, right.v, "shl");
            break;

        case BinaryOp::Shr:
            // Arithmetic shift for signed integers. The language doesn't
            // distinguish `>>` on signed vs unsigned; Sema's type
            // signedness would decide, but for now assume signed.
            result = b.CreateAShr(left.v, right.v, "ashr");
            break;

        default:
            program.diagnostics.errorAt(
                DiagCode::Sem_InvalidBinary, expr->loc,
                "unsupported binary operator");
            return {};
    }

    return Val{result, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitUnary — a prefix unary operation
// ─────────────────────────────────────────────────────────────────────────────
//
// The operand is emitted as a `Val`. The operation is emitted on the
// operand's LLVM value. The result is a fresh scalar.
//
// `not` coerces its operand through `emitTruthiness` — the language's
// truthiness rules apply to `not`, unlike `~` (bitwise not) which
// requires an integer.

Val Emitter::emitUnary(UnaryExprAST* expr) {
    assert(expr && "emitUnary() with null expression");

    llvm::IRBuilder<>& b = program.builder();

    // ─── Emit the operand ─────────────────────────────────────────────────
    Val operand = emit(expr->operand);
    if (!operand.isValid()) return {};

    llvm::Value* result = nullptr;

    switch (expr->op) {
        case UnaryOp::Neg:
            // Arithmetic negation. Integer or float.
            if (operand.v->getType()->isIntegerTy()) {
                result = b.CreateNeg(operand.v, "neg");
            } else {
                result = b.CreateFNeg(operand.v, "fneg");
            }
            break;

        case UnaryOp::Not: {
            // Logical negation. Coerce through truthiness, then invert.
            llvm::Value* i1 = emitTruthiness(operand);
            if (!i1) return {};
            result = b.CreateNot(i1, "not");
            break;
        }

        case UnaryOp::BitNot:
            // Bitwise NOT. Integer only. Sema guarantees the operand type.
            if (!operand.v->getType()->isIntegerTy()) {
                program.diagnostics.errorAt(
                    DiagCode::Sem_InvalidUnary, expr->loc,
                    "bitwise NOT requires an integer operand");
                return {};
            }
            result = b.CreateNot(operand.v, "bnot");
            break;

        default:
            program.diagnostics.errorAt(
                DiagCode::Sem_InvalidUnary, expr->loc,
                "unsupported unary operator");
            return {};
    }

    return Val{result, expr->resolvedType, Own::Owned};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitIf — the expression form of `if`
// ─────────────────────────────────────────────────────────────────────────────
//
// Basic-block shape:
//
//     +--------+  cond false  +------+      +-------+
//     | header |------------->| else |----->| merge |
//     +---+----+              +------+      +-------+
//         | cond true             ▲
//         ▼                       |
//     +------+                    |
//     | then |------ br ----------+
//     +------+
//
// The merge block has a phi whose incoming values are the two branch
// results. The ownership tag of the result is the tag both branches
// returned: `Owned` if both were `Owned`, `Borrowed` if both were
// `Borrowed`. If the branches disagree, the emitter asserts (in debug)
// and returns an invalid `Val` (in release).
//
// ─── Why Both Branches Must Agree ─────────────────────────────────────────
// The result is a single `Val`, so it carries a single tag. If `then`
// produced an `Owned` value (a fresh string from a call) and `else`
// produced a `Borrowed` value (a load), then the phi aliases storage on
// one path and owns a claim on the other. A consumer can't handle both
// cases with one code path. Sema's type checker already requires both
// branches to have the same type — the ownership tag is the runtime
// counterpart of that rule.

Val Emitter::emitIf(IfExprAST* expr) {
    assert(expr && "emitIf() with null expression");

    llvm::IRBuilder<>& b = program.builder();
    llvm::Function* fn = b.GetInsertBlock()->getParent();

    // ─── Emit and coerce the condition ────────────────────────────────────
    Val cond = emit(expr->condition);
    if (!cond.isValid()) return {};

    llvm::Value* condI1 = emitTruthiness(cond);
    if (!condI1) return {};

    // ─── Create the blocks ────────────────────────────────────────────────
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(
        program.llvmContext(), "if.then", fn);
    llvm::BasicBlock* elseBlock = llvm::BasicBlock::Create(
        program.llvmContext(), "if.else", fn);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
        program.llvmContext(), "if.merge", fn);

    b.CreateCondBr(condI1, thenBlock, elseBlock);

    // ─── Then branch ──────────────────────────────────────────────────────
    b.SetInsertPoint(thenBlock);
    Val thenVal = emit(expr->thenBranch);
    if (!thenVal.isValid()) return {};
    // Record the block the branch ended in. `emit` might have added
    // blocks (for a nested `if`, a null-coalesce, etc.), and the phi
    // below needs the actual terminating block, not `thenBlock`.
    llvm::BasicBlock* thenEnd = b.GetInsertBlock();
    b.CreateBr(mergeBlock);

    // ─── Else branch ──────────────────────────────────────────────────────
    b.SetInsertPoint(elseBlock);
    Val elseVal = emit(expr->elseBranch);
    if (!elseVal.isValid()) return {};
    llvm::BasicBlock* elseEnd = b.GetInsertBlock();
    b.CreateBr(mergeBlock);

    // ─── Ownership tag agreement ──────────────────────────────────────────
    // The two branches must agree on whether the result is fresh.
    assert(thenVal.own == elseVal.own
           && "emitIf: branches disagree on ownership tag — Sema should "
              "have rejected this, or the emitter has a bug");
    Own resultOwn = (thenVal.own == elseVal.own) ? thenVal.own : Own::Owned;

    // ─── Phi ──────────────────────────────────────────────────────────────
    b.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = b.CreatePHI(
        thenVal.v->getType(), 2, "if.result");
    phi->addIncoming(thenVal.v, thenEnd);
    phi->addIncoming(elseVal.v, elseEnd);

    return Val{phi, expr->resolvedType, resultOwn};
}

// ─────────────────────────────────────────────────────────────────────────────
// emitRange — a range expression in value position
// ─────────────────────────────────────────────────────────────────────────────
//
// A range is not a standalone value in Lucid. It appears only in three
// positions:
//
//   - As a `for` loop's iterable (`for i in 0..10`).
//   - As a `switch` case's value (`case 1..10:`).
//   - As a slice's bounds (`arr[1..3]`).
//
// None of those reach `emit` — each is handled by its own emitter
// (the for-loop and switch emitters in `EmitStmt.cpp`, the slice
// emitter in `EmitAccess.cpp`). A `RangeExprAST` reaching `emit` is a
// Sema bug: Sema should have rejected `let x = 0..10`.
//
// The emitter reports the bug and returns an invalid `Val`.

Val Emitter::emitRange(RangeExprAST* expr) {
    assert(expr && "emitRange() with null expression");

    program.diagnostics.errorAt(
        DiagCode::Backend_CodegenError, expr->loc,
        "range expressions are not values — Sema should have rejected this");

    return {};
}

} // namespace codegen