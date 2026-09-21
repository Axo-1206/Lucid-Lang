/// @file codegen/intrinsic/LucidIntrinsicEmitter.cpp
/// @brief Implementation of the Lucid-side intrinsic emitters.
///
/// ─── What This File Emits ─────────────────────────────────────────────────
/// The intrinsics CodeGen implements directly, without going through
/// LLVM's own intrinsic machinery. Three families:
///
///   - Compile-time: `#sizeof`, `#alignof`, `#typeof`, `#nameof`.
///     Normally folded by Sema's ConstEvaluator; if one reaches the
///     emitter, it is evaluated here against the `DataLayout`.
///
///   - Runtime-backed: `#tostr`, `#ptrstr`, `#str_len`, `#str_ptr`,
///     `#str_from_ptr`, `#str_concat`, `#str_slice`, `#str_eq`,
///     `#str_byte_at`, `#alloc`, `#free`. These call the runtime ABI
///     through `Abi`. Every one that produces a `lucid.String` uses the
///     out-pointer convention: the caller allocates the result slot and
///     passes its address.
///
///   - Pointer/control: `#addrof`, `#toRef`, `#toPtr`, `#ptrOffset`,
///     `#ptrDiff`, `#likely`, `#unlikely`, `#scope_exit`. These do
///     pointer arithmetic, boundary crossing, or deferred-callback
///     registration.
///
/// ─── F1a: `#tostr` Recursion Stays on `llvm::Value*` ──────────────────────
/// The `#tostr` recursion builds intermediate string values whose AST
/// type is always `string` but which have no natural `TypeAST*` of their
/// own. Threading a `Val` through the recursion would require synthesizing
/// and caching a `PrimitiveTypeAST(PrimitiveKind::String)` somewhere on
/// `ProgramState` or `Types`.
///
/// For pass 1, the recursion's internal helpers return `llvm::Value*`, and
/// only the public `emitTostr` wraps the final result in a `Val` with
/// `expr->resolvedType` as the type. See `TODO(F1b)` near `emitTostrValue`
/// for the eventual fix.
///
/// ─── Special-Case Argument Lowering ───────────────────────────────────────
/// Most intrinsics lower their arguments uniformly: evaluate each, pass
/// the resulting `llvm::Value*`. A handful do not:
///
///   - `#sizeof(T)`, `#alignof(T)`, `#typeof(T)`: the argument is a type.
///   - `#bitcast(T, x)`: arg 0 is a type, arg 1 is a value.
///   - `#alloc(T, n)`: arg 0 is a type, arg 1 is a value.
///   - `#addrof(x)`, `#ptrstr(x)`: the argument is a place; the emitter
///     must NOT load it. Uses `Emitter::emitPlace`.
///   - `#toRef(ptr)`: a null check branches to a fallback or panic.
///   - `#tostr(x)`: dispatches on the argument's type.
///   - `#scope_exit(f)`: the call site emits nothing (registered by Sema).
///
/// These are dispatched by `emitLucidIntrinsic` on `info.kind` before the
/// general argument-lowering loop. The general path handles everything
/// else.

#include "LucidIntrinsicEmitter.hpp"

#include "codegen/emit/Emitter.hpp"
#include "codegen/Program.hpp"
#include "codegen/Types.hpp"
#include "codegen/Abi.hpp"

#include "core/registry/IntrinsicRegistry.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Anonymous-namespace helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// ─── Type-argument recovery ───────────────────────────────────────────────

/// Recover the `TypeAST*` from an intrinsic's type-argument slot.
///
/// Sema stores type arguments two ways depending on how the parser
/// produced them:
///
///   - A bare type name (`#sizeof(int)`) parses as an
///     `IdentifierExprAST` with `isType == true` and `resolvedTypeNode`
///     set to the resolved `TypeAST`.
///
///   - A compound type expression (`#sizeof(Vec2?)`) parses normally, and
///     the resolved type is on `arg->resolvedType`.
///
/// This helper checks both. It is a direct port of the old free function
/// of the same name.
TypeAST* resolveIntrinsicTypeArg(ExprAST* arg) {
    if (!arg) return nullptr;
    if (arg->isa<IdentifierExprAST>()) {
        IdentifierExprAST* id = arg->as<IdentifierExprAST>();
        if (id->isType && id->resolvedTypeNode) {
            return id->resolvedTypeNode;
        }
    }
    return arg->resolvedType;
}

// ─── Diagnostics helpers ──────────────────────────────────────────────────

/// Emit an "intrinsic '#X' requires N argument(s)" diagnostic and return
/// an invalid `Val`.
Val argCountError(IntrinsicCallExprAST* expr,
                  const char* expected,
                  Emitter& emitter) {
    emitter.program.diagnostics.errorAt(
        DiagCode::Sem_ArgCountMismatch, expr->loc,
        "intrinsic '#", emitter.program.pool.lookup(expr->intrinsicName),
        "' requires ", expected, " argument(s)");
    return {};
}

/// Emit an "intrinsic '#X' is not yet implemented" diagnostic and return
/// an invalid `Val`. Used for the cases we know are Sema-valid but haven't
/// ported yet.
Val notImplementedError(IntrinsicCallExprAST* expr,
                        const char* detail,
                        Emitter& emitter) {
    emitter.program.diagnostics.errorAt(
        DiagCode::Backend_CodegenError, expr->loc,
        "intrinsic '#", emitter.program.pool.lookup(expr->intrinsicName),
        "' is not yet implemented in the new codegen: ", detail);
    return {};
}

// ─── `lucid.String` result wrapping ───────────────────────────────────────

/// Wrap a freshly-loaded `lucid.String` SSA value into a `Val` typed as
/// `expr->resolvedType` and tagged `Own::Owned`.
///
/// The `resolvedType` on a `#tostr`/`#str_concat`/etc. call is the AST type
/// for `string` (a `PrimitiveTypeAST(PrimitiveKind::String)`), set by Sema.
Val wrapStringResult(llvm::Value* strValue,
                     IntrinsicCallExprAST* expr) {
    Val out;
    out.v = strValue;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

// ─── Runtime string helpers (out-pointer convention) ──────────────────────

/// Call `__lucid_str_concat(out, a, b)` and return the loaded result.
///
/// Internal helper: returns `llvm::Value*` (the loaded `lucid.String`),
/// not a `Val`, so the `#tostr` recursion can chain concatenations without
/// threading a `TypeAST*` through every intermediate. See `TODO(F1b)`.
llvm::Value* emitStrConcatRaw(llvm::Value* a,
                              llvm::Value* b,
                              Emitter& emitter) {
    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::StructType* strTy = emitter.program.types().stringType();

    llvm::AllocaInst* out = emitter.createEntryAlloca(strTy, "concat_out");
    llvm::AllocaInst* aSlot = emitter.createEntryAlloca(strTy, "concat_a");
    llvm::AllocaInst* bSlot = emitter.createEntryAlloca(strTy, "concat_b");
    irb.CreateStore(a, aSlot);
    irb.CreateStore(b, bSlot);

    emitter.program.abi().StrConcat(irb, out, aSlot, bSlot);

    return irb.CreateLoad(strTy, out, "concat_result");
}

/// Call the appropriate formatter for a scalar value and return the loaded
/// `lucid.String`. `kind` drives the dispatch, mirroring the old
/// `emitIntToStr`/`emitBoolToStr`/etc. helpers but collapsed into one
/// function because the out-pointer pattern is identical for all six.
///
/// Returns null if the primitive kind is not a formattable scalar (e.g.
/// `String`, which the caller handles as identity).
llvm::Value* emitScalarToStr(llvm::Value* value,
                             PrimitiveKind kind,
                             Emitter& emitter) {
    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::LLVMContext& ctx = emitter.program.llvmContext();
    llvm::StructType* strTy = emitter.program.types().stringType();
    Abi& abi = emitter.program.abi();

    llvm::AllocaInst* out = emitter.createEntryAlloca(strTy, "tostr_out");

    switch (kind) {
        case PrimitiveKind::Bool: {
            // Runtime takes I1 (one byte). Lucid `bool` is already i1 in
            // the generated IR; the ABI's `I1` tag is `i1 zeroext`, so
            // pass the i1 straight through — `Abi::emitCall` asserts the
            // LLVM type matches and the declaration carries `zeroext`.
            abi.BoolToStr(irb, out, value);
            break;
        }
        case PrimitiveKind::Char: {
            // `LucidI32` on the runtime side.
            llvm::Type* i32Ty = llvm::Type::getInt32Ty(ctx);
            llvm::Value* asI32 = value->getType() == i32Ty
                ? value
                : irb.CreateZExtOrTrunc(value, i32Ty, "char_to_i32");
            abi.CharToStr(irb, out, asI32);
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
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::Value* asI64 = value->getType() == i64Ty
                ? value
                : irb.CreateSExt(value, i64Ty, "int_to_i64");
            abi.IntToStr(irb, out, asI64);
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
            llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
            llvm::Value* asI64 = value->getType() == i64Ty
                ? value
                : irb.CreateZExt(value, i64Ty, "uint_to_i64");
            abi.UintToStr(irb, out, asI64);
            break;
        }
        case PrimitiveKind::Float:
        case PrimitiveKind::Double:
        case PrimitiveKind::Decimal: {
            llvm::Type* f64Ty = llvm::Type::getDoubleTy(ctx);
            llvm::Value* asF64 = value->getType() == f64Ty
                ? value
                : irb.CreateFPCast(value, f64Ty, "float_to_f64");
            abi.FloatToStr(irb, out, asF64);
            break;
        }
        case PrimitiveKind::String:
            // Caller handles identity.
            return nullptr;
    }

    return irb.CreateLoad(strTy, out, "tostr_result");
}

// ─── Field-access path reconstruction (for `#tostr` of a function value) ──

/// Build a dotted source path for a `FieldAccessExprAST` chain, e.g.
/// `player.weapon.str`. Used by `#tostr` when the argument is a function
/// value and we want to print its declared name.
std::string getFieldAccessPath(FieldAccessExprAST* field,
                               Emitter& emitter) {
    StringPool& pool = emitter.program.pool;
    std::string path = pool.lookup(field->fieldName);

    ExprAST* obj = field->object;
    while (obj) {
        if (obj->isa<IdentifierExprAST>()) {
            IdentifierExprAST* id = obj->as<IdentifierExprAST>();
            path = pool.lookup(id->name) + "." + path;
            break;
        } else if (obj->isa<FieldAccessExprAST>()) {
            FieldAccessExprAST* parent = obj->as<FieldAccessExprAST>();
            path = pool.lookup(parent->fieldName) + "." + path;
            obj = parent->object;
        } else if (obj->isa<ModuleAccessExprAST>()) {
            ModuleAccessExprAST* mod = obj->as<ModuleAccessExprAST>();
            path = pool.lookup(mod->moduleName) + ":" + path;
            break;
        } else {
            break;
        }
    }

    return path;
}

// ─────────────────────────────────────────────────────────────────────────────
// `#tostr` recursive value formatter (F1a)
// ─────────────────────────────────────────────────────────────────────────────
//
// `emitTostrValue` returns `llvm::Value*` — the SSA value of a
// `lucid.String` — not a `Val`. The recursion into struct fields, the
// concatenation chain, and the enum switch all operate on raw
// `llvm::Value*`.
//
// `TODO(F1b)`: when a `string` AST type is cheaply reachable (e.g. cached
// on `ProgramState` or `Types`), switch this to return `Val` with
// `Own::Owned` so the recursion participates in the ownership model
// uniformly. For now, every value this function produces is a freshly
// allocated heap string (`Owned` by construction), so the eventual wrap at
// `emitTostr` is correct.
//
// `sourceExpr` is the syntactic expression the value came from. It is
// meaningful only for function/closure values (to recover a declared name)
// and is `nullptr` for struct fields read out of an aggregate.
llvm::Value* emitTostrValue(llvm::Value* val,
                            TypeAST* type,
                            ExprAST* sourceExpr,
                            SourceLocation loc,
                            Emitter& emitter);

/// Emit `#tostr` for a function-typed value.
///
/// Recovers the declared name from the source expression: an identifier
/// gives its own name, a field access chain gives a dotted path, a module
/// access gives `mod:member`. Anything else prints `<closure>`.
llvm::Value* emitTostrForFunction(llvm::Value* /*val*/,
                                  ExprAST* sourceExpr,
                                  Emitter& emitter) {
    StringPool& pool = emitter.program.pool;
    std::string nameStr;

    if (sourceExpr) {
        if (sourceExpr->isa<IdentifierExprAST>()) {
            nameStr = pool.lookup(sourceExpr->as<IdentifierExprAST>()->name);
        } else if (sourceExpr->isa<FieldAccessExprAST>()) {
            nameStr = getFieldAccessPath(
                sourceExpr->as<FieldAccessExprAST>(), emitter);
        } else if (sourceExpr->isa<ModuleAccessExprAST>()) {
            ModuleAccessExprAST* mod = sourceExpr->as<ModuleAccessExprAST>();
            nameStr = pool.lookup(mod->moduleName) + ":"
                    + pool.lookup(mod->memberName);
        } else {
            nameStr = "<closure>";
        }
    } else {
        nameStr = "<closure>";
    }

    return emitter.program.types().stringLiteral(nameStr,
                                                 emitter.program.builder());
}

/// Emit `#tostr` for an enum value: a switch over the variants, each
/// producing `"EnumName.VariantName"`, with a numeric fallback for an
/// unknown value.
llvm::Value* emitTostrForEnum(llvm::Value* val,
                              EnumDeclAST* enumDecl,
                              Emitter& emitter) {
    llvm::IRBuilder<>& irb = emitter.program.builder();
    StringPool& pool = emitter.program.pool;
    llvm::StructType* strTy = emitter.program.types().stringType();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(emitter.program.llvmContext());

    llvm::Function* func = irb.GetInsertBlock()->getParent();

    llvm::BasicBlock* defaultBlock = llvm::BasicBlock::Create(
        emitter.program.llvmContext(), "tostr_enum_default", func);
    llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
        emitter.program.llvmContext(), "tostr_enum_merge", func);

    llvm::SwitchInst* sw = irb.CreateSwitch(
        val, defaultBlock,
        static_cast<unsigned>(enumDecl->variants.size()));

    // Each (incoming block, incoming value) pair feeds the merge PHI.
    std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>> incoming;

    std::string enumName = pool.lookup(enumDecl->name);

    for (EnumVariantAST* variant : enumDecl->variants) {
        if (!variant) continue;

        // Variants carry their explicit integer values in the AST.
        llvm::ConstantInt* caseVal = llvm::ConstantInt::get(
            val->getType(), static_cast<uint64_t>(variant->value));

        llvm::BasicBlock* caseBlock = llvm::BasicBlock::Create(
            emitter.program.llvmContext(),
            "tostr_enum_" + pool.lookup(variant->name), func);
        sw->addCase(caseVal, caseBlock);

        irb.SetInsertPoint(caseBlock);
        std::string label = enumName + "." + pool.lookup(variant->name);
        incoming.push_back({
            caseBlock,
            emitter.program.types().stringLiteral(label, irb)
        });
        irb.CreateBr(mergeBlock);
    }

    // Default: print the numeric value, so an out-of-range enum doesn't
    // produce garbage.
    irb.SetInsertPoint(defaultBlock);
    llvm::Value* asI64 = val->getType() == i64Ty
        ? val
        : irb.CreateSExtOrTrunc(val, i64Ty, "tostr_enum_i64");

    llvm::AllocaInst* outSlot = emitter.createEntryAlloca(
        strTy, "tostr_enum_default_out");
    emitter.program.abi().IntToStr(irb, outSlot, asI64);
    llvm::Value* defaultStr = irb.CreateLoad(
        strTy, outSlot, "tostr_enum_default_str");
    incoming.push_back({defaultBlock, defaultStr});
    irb.CreateBr(mergeBlock);

    irb.SetInsertPoint(mergeBlock);
    llvm::PHINode* phi = irb.CreatePHI(
        strTy, static_cast<unsigned>(incoming.size()),
        "tostr_enum_result");
    for (auto& [block, value] : incoming) {
        phi->addIncoming(value, block);
    }
    return phi;
}

/// Emit `#tostr` for a struct value: either a custom `str` override, or a
/// synthesized `"Name{ f1: v1, f2: v2 }"`.
///
/// `val` may be a pointer to the struct or a struct value. The helper
/// normalizes both shapes via `readField`.
llvm::Value* emitTostrForStruct(llvm::Value* val,
                                StructDeclAST* structDecl,
                                SourceLocation loc,
                                Emitter& emitter) {
    llvm::IRBuilder<>& irb = emitter.program.builder();
    StringPool& pool = emitter.program.pool;
    llvm::StructType* strTy = emitter.program.types().stringType();
    llvm::StructType* structTy = emitter.program.types().structType(structDecl);

    if (!structTy) {
        emitter.program.diagnostics.errorAt(
            DiagCode::Backend_CodegenError, loc,
            "struct '", pool.lookup(structDecl->name),
            "' has no LLVM type");
        return llvm::Constant::getNullValue(strTy);
    }

    // ─── `readField(i)` — load field `i` regardless of val's shape ────────
    auto readField = [&](size_t index) -> llvm::Value* {
        if (val->getType()->isPointerTy()) {
            llvm::Type* fieldTy = structTy->getElementType(index);
            llvm::Value* fieldPtr = irb.CreateStructGEP(
                structTy, val, static_cast<unsigned>(index),
                "tostr_field_ptr");
            return irb.CreateLoad(fieldTy, fieldPtr, "tostr_field_load");
        }
        return irb.CreateExtractValue(
            val, static_cast<unsigned>(index), "tostr_field_val");
    };

    // ─── `str` override: a zero-arg function returning string ────────────
    InternedString strFieldName = pool.intern("str");
    size_t strIndex = structDecl->indexOfField(strFieldName);
    if (strIndex != SIZE_MAX) {
        FieldDeclAST* strField = structDecl->fields[strIndex];
        bool isValidOverride = strField->type
                            && strField->type->isa<FuncTypeAST>();
        if (isValidOverride) {
            FuncTypeAST* fnType = strField->type->as<FuncTypeAST>();
            isValidOverride = fnType->params.empty()
                && fnType->returnType
                && fnType->returnType->isa<PrimitiveTypeAST>()
                && fnType->returnType->as<PrimitiveTypeAST>()->primitiveKind
                    == PrimitiveKind::String;
        }
        if (isValidOverride) {
            llvm::Value* closureVal = readField(strIndex);
            llvm::Value* funcPtr = irb.CreateExtractValue(
                closureVal, 0, "str_override_func");
            llvm::Value* envPtr = irb.CreateExtractValue(
                closureVal, 1, "str_override_env");
            llvm::Value* result = emitter.emitClosureCall(
                funcPtr, envPtr, /*args=*/{}, strTy);
            return result ? result : llvm::Constant::getNullValue(strTy);
        }
    }

    // ─── Synthesize "Name{ f1: v1, f2: v2 }" ──────────────────────────────
    if (structDecl->fields.empty()) {
        return emitter.program.types().stringLiteral(
            pool.lookup(structDecl->name) + "{}", irb);
    }

    llvm::Value* result = emitter.program.types().stringLiteral(
        pool.lookup(structDecl->name) + "{ ", irb);

    for (size_t i = 0; i < structDecl->fields.size(); ++i) {
        FieldDeclAST* field = structDecl->fields[i];
        if (!field) continue;

        std::string fieldPrefix = pool.lookup(field->name) + ": ";

        llvm::Value* fieldVal = readField(i);
        llvm::Value* fieldStr = emitTostrValue(
            fieldVal, field->type, /*sourceExpr=*/nullptr, loc, emitter);
        if (!fieldStr) {
            fieldStr = llvm::Constant::getNullValue(strTy);
        }

        result = emitStrConcatRaw(
            result,
            emitter.program.types().stringLiteral(fieldPrefix, irb),
            emitter);
        result = emitStrConcatRaw(result, fieldStr, emitter);

        if (i + 1 < structDecl->fields.size()) {
            result = emitStrConcatRaw(
                result,
                emitter.program.types().stringLiteral(", ", irb),
                emitter);
        }
    }

    result = emitStrConcatRaw(
        result,
        emitter.program.types().stringLiteral(" }", irb),
        emitter);

    return result;
}

/// The recursive `#tostr` core. Dispatches on the argument's AST type.
llvm::Value* emitTostrValue(llvm::Value* val,
                            TypeAST* type,
                            ExprAST* sourceExpr,
                            SourceLocation loc,
                            Emitter& emitter) {
    llvm::IRBuilder<>& irb = emitter.program.builder();
    StringPool& pool = emitter.program.pool;
    llvm::StructType* strTy = emitter.program.types().stringType();

    if (!type) {
        return emitter.program.types().stringLiteral("<unknown>", irb);
    }

    // ─── Function values: print the declared name ────────────────────────
    if (type->isa<FuncTypeAST>()) {
        return emitTostrForFunction(val, sourceExpr, emitter);
    }

    // ─── Primitives ───────────────────────────────────────────────────────
    if (type->isa<PrimitiveTypeAST>()) {
        PrimitiveKind kind = type->as<PrimitiveTypeAST>()->primitiveKind;

        if (kind == PrimitiveKind::String) {
            // Identity: the value is already a string. No copy, no
            // ownership transfer. The caller (`emitTostr`) wraps it.
            return val;
        }

        llvm::Value* formatted = emitScalarToStr(val, kind, emitter);
        if (formatted) return formatted;

        return emitter.program.types().stringLiteral(
            "<unknown primitive>", irb);
    }

    // ─── Named types: enum or struct ──────────────────────────────────────
    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        if (!named->resolvedDecl) {
            return emitter.program.types().stringLiteral(
                "<" + pool.lookup(named->name) + ">", irb);
        }

        if (named->resolvedDecl->isa<TraitDeclAST>()) {
            // Safety net: Sema should have rejected this.
            return emitter.program.types().stringLiteral(
                "<trait " + pool.lookup(named->name) + ">", irb);
        }

        if (named->resolvedDecl->isa<EnumDeclAST>()) {
            return emitTostrForEnum(
                val, named->resolvedDecl->as<EnumDeclAST>(), emitter);
        }

        if (named->resolvedDecl->isa<StructDeclAST>()) {
            return emitTostrForStruct(
                val, named->resolvedDecl->as<StructDeclAST>(), loc, emitter);
        }
    }

    // ─── Fallback ─────────────────────────────────────────────────────────
    std::string typeName = emitter.program.types().typeName(type);
    return emitter.program.types().stringLiteral(
        "<" + typeName + ">", irb);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Special-case emitters
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// `#scope_exit(f, args)` — no-op at the call site.
///
/// Sema has already lifted the call into a `ScopeExitRegistration` on the
/// enclosing block. `Emitter::dropScopeAlive` runs the registrations in
/// reverse order at block exit.
Val emitScopeExit(IntrinsicCallExprAST* /*expr*/, Emitter& /*emitter*/) {
    return {};
}

/// `#sizeof(T) -> uint64`. Normally folded by Sema; if it reaches the
/// emitter, evaluate against the `DataLayout`.
Val emitSizeof(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1 (type)", emitter);
    }

    TypeAST* targetTy = resolveIntrinsicTypeArg(expr->args[0]);
    if (!targetTy) {
        emitter.program.diagnostics.errorAt(
            DiagCode::Sem_TypeMismatch, expr->loc,
            "intrinsic '#sizeof' could not resolve its type argument");
        return {};
    }

    uint64_t size = emitter.program.types().sizeOf(targetTy);
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(emitter.program.llvmContext());
    llvm::Value* result = llvm::ConstantInt::get(i64Ty, size);

    Val out;
    out.v = result;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

/// `#alignof(T) -> uint64`.
Val emitAlignof(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1 (type)", emitter);
    }

    TypeAST* targetTy = resolveIntrinsicTypeArg(expr->args[0]);
    if (!targetTy) {
        emitter.program.diagnostics.errorAt(
            DiagCode::Sem_TypeMismatch, expr->loc,
            "intrinsic '#alignof' could not resolve its type argument");
        return {};
    }

    uint64_t align = emitter.program.types().alignOf(targetTy);
    if (align == 0) align = 1;

    llvm::Type* i64Ty = llvm::Type::getInt64Ty(emitter.program.llvmContext());
    llvm::Value* result = llvm::ConstantInt::get(i64Ty, align);

    Val out;
    out.v = result;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

/// `#typeof(x) -> string`. Folded by Sema in the common case; if it
/// reaches the emitter, the argument's resolved type is still on the AST.
Val emitTypeof(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1", emitter);
    }

    TypeAST* ty = resolveIntrinsicTypeArg(expr->args[0]);
    std::string name = ty ? emitter.program.types().typeName(ty)
                          : std::string("unknown");

    llvm::Value* str = emitter.program.types().stringLiteral(
        name, emitter.program.builder());
    return wrapStringResult(str, expr);
}

/// `#nameof(x) -> string`. Same shape as `#typeof`.
Val emitNameof(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1", emitter);
    }

    StringPool& pool = emitter.program.pool;
    ExprAST* arg = expr->args[0];
    std::string nameStr = "unknown";

    if (arg->isa<IdentifierExprAST>()) {
        nameStr = pool.lookup(arg->as<IdentifierExprAST>()->name);
    } else if (arg->isa<FieldAccessExprAST>()) {
        nameStr = pool.lookup(arg->as<FieldAccessExprAST>()->fieldName);
    } else if (arg->isa<ModuleAccessExprAST>()) {
        nameStr = pool.lookup(arg->as<ModuleAccessExprAST>()->memberName);
    }

    llvm::Value* str = emitter.program.types().stringLiteral(
        nameStr, emitter.program.builder());
    return wrapStringResult(str, expr);
}

/// `#bitcast(T, x) -> T`. Reinterpret `x` as type `T`.
///
/// Sema has already verified the sizes match, so this emitter only needs
/// to produce the cast.
Val emitBitcast(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (type, value)", emitter);
    }

    // Arg 0 is a type; arg 1 is the value.
    Val argVal = emitter.emit(expr->args[1]);
    if (!argVal.isValid()) return {};

    llvm::Type* targetTy = emitter.program.types().get(expr->resolvedType);
    if (!targetTy) {
        emitter.program.diagnostics.errorAt(
            DiagCode::Sem_TypeMismatch, expr->loc,
            "intrinsic '#bitcast' could not resolve its target type");
        return {};
    }

    llvm::Value* casted = emitter.program.builder().CreateBitCast(
        argVal.v, targetTy, "bitcast");

    Val out;
    out.v = casted;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

/// `#alloc(T, n) -> *T`. Allocate `n * sizeof(T)` bytes through
/// `__lucid_alloc`, cast the result to `*T`.
///
/// Arg 0 is the element type; arg 1 is the count.
Val emitAlloc(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (type, count)", emitter);
    }

    // ─── Element size from the target pointer's pointee ───────────────────
    uint64_t elemSize = 1;
    TypeAST* targetTy = expr->resolvedType;
    if (targetTy && targetTy->isa<PtrTypeAST>()) {
        TypeAST* pointee = targetTy->as<PtrTypeAST>()->inner;
        uint64_t resolvedSize = emitter.program.types().sizeOf(pointee);
        if (resolvedSize > 0) elemSize = resolvedSize;
    }

    // ─── Count ────────────────────────────────────────────────────────────
    Val countVal = emitter.emit(expr->args[1]);
    if (!countVal.isValid()) return {};

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(emitter.program.llvmContext());

    llvm::Value* count = countVal.v;
    if (count->getType() != i64Ty) {
        count = irb.CreateIntCast(count, i64Ty, /*isSigned=*/false,
                                  "alloc_count");
    }

    llvm::Value* size = irb.CreateMul(
        count, llvm::ConstantInt::get(i64Ty, elemSize), "alloc_size");

    // ─── Call `__lucid_alloc(size)` ───────────────────────────────────────
    llvm::Value* raw = emitter.program.abi().Alloc(irb, size);
    if (!raw) return {};

    // ─── Cast to the target pointer type ──────────────────────────────────
    llvm::Type* targetLLVMTy = emitter.program.types().get(targetTy);
    llvm::Value* result = raw;
    if (targetLLVMTy && targetLLVMTy->isPointerTy()
        && raw->getType() != targetLLVMTy) {
        result = irb.CreatePointerCast(raw, targetLLVMTy, "alloc_result");
    }

    Val out;
    out.v = result;
    out.ty = targetTy;
    out.own = Own::Owned;
    return out;
}

/// `#addrof(x) -> *T`. The address of the place `x` names.
///
/// Uses `Emitter::emitPlace` (friend access), which returns the pointer to
/// storage rather than the loaded value. Sema guarantees the argument is
/// an lvalue.
Val emitAddrof(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1 (lvalue)", emitter);
    }

    Place place = emitter.emitPlace(expr->args[0]);
    if (!place.isValid()) {
        emitter.program.diagnostics.errorAt(
            DiagCode::Sem_InvalidParamType, expr->args[0]->loc,
            "intrinsic '#addrof' argument is not an lvalue");
        return {};
    }

    Val out;
    out.v = place.ptr;
    out.ty = expr->resolvedType;
    out.own = Own::Borrowed;  // The address aliases the place.
    return out;
}

/// `#ptrstr(x) -> string`. Format the address of `x` as a hex string.
///
/// Uses `emitPlace` to get the address, then calls
/// `__lucid_ptr_to_hex_string`.
Val emitPtrstr(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1 (lvalue)", emitter);
    }

    Place place = emitter.emitPlace(expr->args[0]);
    if (!place.isValid()) {
        emitter.program.diagnostics.errorAt(
            DiagCode::Sem_InvalidParamType, expr->args[0]->loc,
            "intrinsic '#ptrstr' argument is not an lvalue");
        return {};
    }

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::StructType* strTy = emitter.program.types().stringType();
    llvm::AllocaInst* out = emitter.createEntryAlloca(strTy, "ptrstr_out");

    // `PtrToHexString(StrPtr out, Ptr value)`.
    // The pointer is opaque already; `Abi::emitCall` asserts the LLVM
    // type matches, and with opaque pointers `place.ptr` is `ptr`.
    emitter.program.abi().PtrToHexString(irb, out, place.ptr);

    llvm::Value* result = irb.CreateLoad(strTy, out, "ptrstr_result");
    return wrapStringResult(result, expr);
}

/// `#toRef(ptr) -> &T`. Assert a raw pointer is non-null.
///
/// On null, the old code branched to the enclosing `??` fallback if one
/// was active, or to a panic otherwise. The new `Emitter` has no
/// null-coalesce state yet, so this always panics.
///
/// `TODO(null-coalesce)`: see `Emitter.hpp`. When
/// `Emitter::insideNullCoalesce()` and
/// `Emitter::nullCoalesceFallbackBlock()` exist, replace the unconditional
/// panic branch below with the old conditional.
Val emitToRef(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1", emitter);
    }

    Val argVal = emitter.emit(expr->args[0]);
    if (!argVal.isValid()) return {};

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::LLVMContext& ctx = emitter.program.llvmContext();

    // Cast to an opaque pointer so `isNull` is uniform regardless of the
    // argument's static pointer type.
    llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
    llvm::Value* checked = argVal.v;
    if (checked->getType() != ptrTy) {
        checked = irb.CreatePointerCast(checked, ptrTy, "toRef_cast");
    }

    llvm::Value* isNull = irb.CreateIsNull(checked, "toRef_is_null");

    llvm::Function* func = irb.GetInsertBlock()->getParent();
    llvm::BasicBlock* continueBlock = llvm::BasicBlock::Create(
        ctx, "toRef_continue", func);
    llvm::BasicBlock* panicBlock = llvm::BasicBlock::Create(
        ctx, "toRef_panic", func);

    irb.CreateCondBr(isNull, panicBlock, continueBlock);

    irb.SetInsertPoint(panicBlock);
    emitter.emitPanic(RuntimeErrorKind::NullPointerDereference, expr->loc);
    // emitPanic is expected to terminate the block with `unreachable`.
    // If it doesn't, add `irb.CreateUnreachable()` here.

    irb.SetInsertPoint(continueBlock);

    Val out;
    out.v = checked;
    out.ty = expr->resolvedType;
    out.own = Own::Borrowed;  // The reference aliases the pointer's target.
    return out;
}

/// `#tostr(x) -> string`. Format `x` as its type's canonical string.
///
/// Uses the F1a recursion: `emitTostrValue` returns an `llvm::Value*`
/// (the SSA value of a `lucid.String`); this wrapper produces the `Val`.
///
/// The one ownership subtlety: if `x` is already a string, the recursion
/// returns the input value unchanged, and this wrapper must NOT change its
/// ownership tag. Returning `Owned` on a borrowed string would double-free
/// at the store site.
Val emitTostr(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1", emitter);
    }

    Val argVal = emitter.emit(expr->args[0]);
    if (!argVal.isValid()) return {};

    TypeAST* argTy = expr->args[0]->resolvedType;

    // ─── Identity: string argument passes through, ownership preserved ────
    //
    // This is the ONLY case where `#tostr`'s result is not a fresh heap
    // string. Returning `Own::Owned` on a borrowed argument would make
    // the caller free a value it doesn't own.
    if (argTy && argTy->isa<PrimitiveTypeAST>()
        && argTy->as<PrimitiveTypeAST>()->primitiveKind
            == PrimitiveKind::String) {
        Val out;
        out.v = argVal.v;
        out.ty = expr->resolvedType;
        out.own = argVal.own;  // preserve — do NOT force Owned
        return out;
    }

    // ─── Everything else: recursion produces a fresh heap string ──────────
    llvm::Value* formatted = emitTostrValue(
        argVal.v, argTy, expr->args[0], expr->loc, emitter);
    if (!formatted) return {};

    return wrapStringResult(formatted, expr);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// General Lucid-side emitters
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// `#toPtr(ref) -> *T`. Identity: a reference is already a pointer.
Val emitToPtr(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1", emitter);
    }
    Val argVal = emitter.emit(expr->args[0]);
    if (!argVal.isValid()) return {};

    Val out;
    out.v = argVal.v;
    out.ty = expr->resolvedType;
    // Identity: the pointer aliases the reference. Do not force Owned.
    out.own = argVal.own;
    return out;
}

/// `#ptrOffset(ptr, n) -> *T`. Offset `ptr` by `n` elements of its
/// pointee type.
Val emitPtrOffset(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (ptr, offset)", emitter);
    }

    Val ptrVal = emitter.emit(expr->args[0]);
    Val offsetVal = emitter.emit(expr->args[1]);
    if (!ptrVal.isValid() || !offsetVal.isValid()) return {};

    // Recover the pointee LLVM type from the AST. With opaque pointers,
    // GEP needs the element type explicitly.
    llvm::Type* elemTy = llvm::Type::getInt8Ty(emitter.program.llvmContext());
    TypeAST* ptrTy = expr->args[0]->resolvedType;
    if (ptrTy && ptrTy->isa<PtrTypeAST>()) {
        TypeAST* pointee = ptrTy->as<PtrTypeAST>()->inner;
        if (llvm::Type* resolved = emitter.program.types().get(pointee)) {
            elemTy = resolved;
        }
    }

    llvm::Value* gep = emitter.program.builder().CreateInBoundsGEP(
        elemTy, ptrVal.v, offsetVal.v, "ptr_offset");

    Val out;
    out.v = gep;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

/// `#ptrDiff(p1, p2) -> int64`. Distance between two pointers, in units of
/// the pointee type.
Val emitPtrDiff(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (p1, p2)", emitter);
    }

    Val p1 = emitter.emit(expr->args[0]);
    Val p2 = emitter.emit(expr->args[1]);
    if (!p1.isValid() || !p2.isValid()) return {};

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(emitter.program.llvmContext());

    llvm::Value* a = irb.CreatePtrToInt(p1.v, i64Ty, "ptrdiff_a");
    llvm::Value* b = irb.CreatePtrToInt(p2.v, i64Ty, "ptrdiff_b");
    llvm::Value* diffBytes = irb.CreateSub(a, b, "ptrdiff_bytes");

    // Convert bytes to elements.
    uint64_t elemSize = 1;
    TypeAST* ptrTy = expr->args[0]->resolvedType;
    if (ptrTy && ptrTy->isa<PtrTypeAST>()) {
        TypeAST* pointee = ptrTy->as<PtrTypeAST>()->inner;
        uint64_t resolved = emitter.program.types().sizeOf(pointee);
        if (resolved > 0) elemSize = resolved;
    }

    llvm::Value* result = diffBytes;
    if (elemSize > 1) {
        result = irb.CreateSDiv(
            diffBytes,
            llvm::ConstantInt::get(i64Ty, elemSize),
            "ptrdiff_elems");
    }

    Val out;
    out.v = result;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

/// `#free(ptr) -> void`.
Val emitFree(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1 (ptr)", emitter);
    }

    Val ptrVal = emitter.emit(expr->args[0]);
    if (!ptrVal.isValid()) return {};

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::Type* ptrTy = llvm::PointerType::get(
        emitter.program.llvmContext(), 0);

    llvm::Value* ptr = ptrVal.v;
    if (ptr->getType() != ptrTy) {
        ptr = irb.CreatePointerCast(ptr, ptrTy, "free_cast");
    }

    emitter.program.abi().Free(irb, ptr);
    return {};
}

/// `#str_len(s) -> int64`. Extract field 1 of `lucid.String`.
Val emitStrLen(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1 (string)", emitter);
    }

    Val strVal = emitter.emit(expr->args[0]);
    if (!strVal.isValid()) return {};

    llvm::Value* len = emitter.program.builder().CreateExtractValue(
        strVal.v, 1, "str_len");

    Val out;
    out.v = len;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

/// `#str_ptr(s) -> *int8`. Extract field 0 of `lucid.String`.
Val emitStrPtr(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1 (string)", emitter);
    }

    Val strVal = emitter.emit(expr->args[0]);
    if (!strVal.isValid()) return {};

    llvm::Value* ptr = emitter.program.builder().CreateExtractValue(
        strVal.v, 0, "str_ptr");

    Val out;
    out.v = ptr;
    out.ty = expr->resolvedType;
    out.own = Own::Borrowed;  // The pointer aliases the string's buffer.
    return out;
}

/// `#str_from_ptr(ptr, len) -> string`. Build a `lucid.String` from a raw
/// pointer and a length. The `cap` field is set to `len` (so a drop will
/// free the buffer); the caller is responsible for ensuring `ptr` points
/// into a valid buffer that `__lucid_free` understands.
///
/// TODO: verify the ownership semantics against the runtime — if
/// `str_from_ptr` is meant to borrow rather than own, `cap` should be 0
/// and the buffer would leak. Check `StringRuntime.cpp`.
Val emitStrFromPtr(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (ptr, len)", emitter);
    }

    Val ptrVal = emitter.emit(expr->args[0]);
    Val lenVal = emitter.emit(expr->args[1]);
    if (!ptrVal.isValid() || !lenVal.isValid()) return {};

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(emitter.program.llvmContext());
    llvm::StructType* strTy = emitter.program.types().stringType();

    llvm::Value* len = lenVal.v;
    if (len->getType() != i64Ty) {
        len = irb.CreateIntCast(len, i64Ty, /*isSigned=*/false,
                                "str_from_ptr_len");
    }

    llvm::Value* str = llvm::UndefValue::get(strTy);
    str = irb.CreateInsertValue(str, ptrVal.v, 0, "str_fp_data");
    str = irb.CreateInsertValue(str, len, 1, "str_fp_len");
    str = irb.CreateInsertValue(str, len, 2, "str_fp_cap");

    Val out;
    out.v = str;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

/// `#str_concat(a, b) -> string`.
Val emitStrConcat(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (a, b)", emitter);
    }

    Val a = emitter.emit(expr->args[0]);
    Val b = emitter.emit(expr->args[1]);
    if (!a.isValid() || !b.isValid()) return {};

    llvm::Value* result = emitStrConcatRaw(a.v, b.v, emitter);
    return wrapStringResult(result, expr);
}

/// `#str_slice(s, from, to) -> string`.
Val emitStrSlice(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 3) {
        return argCountError(expr, "3 (s, from, to)", emitter);
    }

    Val s = emitter.emit(expr->args[0]);
    Val from = emitter.emit(expr->args[1]);
    Val to = emitter.emit(expr->args[2]);
    if (!s.isValid() || !from.isValid() || !to.isValid()) return {};

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(emitter.program.llvmContext());
    llvm::StructType* strTy = emitter.program.types().stringType();

    llvm::Value* fromVal = from.v;
    if (fromVal->getType() != i64Ty) {
        fromVal = irb.CreateIntCast(fromVal, i64Ty, /*isSigned=*/true,
                                    "str_slice_from");
    }
    llvm::Value* toVal = to.v;
    if (toVal->getType() != i64Ty) {
        toVal = irb.CreateIntCast(toVal, i64Ty, /*isSigned=*/true,
                                  "str_slice_to");
    }

    llvm::AllocaInst* out = emitter.createEntryAlloca(strTy, "str_slice_out");
    llvm::AllocaInst* sSlot = emitter.createEntryAlloca(strTy, "str_slice_s");
    irb.CreateStore(s.v, sSlot);

    // Signature: StrSlice(StrPtr out, StrPtr s, I64 from, I64 to).
    emitter.program.abi().StrSlice(irb, out, sSlot, fromVal, toVal);

    llvm::Value* result = irb.CreateLoad(strTy, out, "str_slice_result");
    return wrapStringResult(result, expr);
}

/// `#str_eq(a, b) -> bool`.
Val emitStrEq(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (a, b)", emitter);
    }

    Val a = emitter.emit(expr->args[0]);
    Val b = emitter.emit(expr->args[1]);
    if (!a.isValid() || !b.isValid()) return {};

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::StructType* strTy = emitter.program.types().stringType();

    llvm::AllocaInst* aSlot = emitter.createEntryAlloca(strTy, "str_eq_a");
    llvm::AllocaInst* bSlot = emitter.createEntryAlloca(strTy, "str_eq_b");
    irb.CreateStore(a.v, aSlot);
    irb.CreateStore(b.v, bSlot);

    // Signature: StrEq(StrPtr a, StrPtr b) -> I1.
    llvm::Value* result = emitter.program.abi().StrEq(irb, aSlot, bSlot);

    Val out;
    out.v = result;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

/// `#str_byte_at(s, i) -> int8`. Bounds check is Sema's responsibility
/// (Sema inserts the check on the AST before codegen); this emitter just
/// loads the byte.
Val emitStrByteAt(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 2) {
        return argCountError(expr, "2 (s, index)", emitter);
    }

    Val s = emitter.emit(expr->args[0]);
    Val idx = emitter.emit(expr->args[1]);
    if (!s.isValid() || !idx.isValid()) return {};

    llvm::IRBuilder<>& irb = emitter.program.builder();
    llvm::LLVMContext& ctx = emitter.program.llvmContext();

    llvm::Value* dataPtr = irb.CreateExtractValue(s.v, 0, "str_byte_data");

    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    llvm::Value* idxVal = idx.v;
    if (idxVal->getType() != i64Ty) {
        idxVal = irb.CreateIntCast(idxVal, i64Ty, /*isSigned=*/true,
                                   "str_byte_idx");
    }

    llvm::Value* bytePtr = irb.CreateGEP(
        llvm::Type::getInt8Ty(ctx), dataPtr, idxVal, "str_byte_ptr");
    llvm::Value* byte = irb.CreateLoad(
        llvm::Type::getInt8Ty(ctx), bytePtr, "str_byte");

    Val out;
    out.v = byte;
    out.ty = expr->resolvedType;
    out.own = Own::Owned;
    return out;
}

/// `#likely(cond) -> bool`, `#unlikely(cond) -> bool`. Identity; the
/// branch-weight metadata is a later pass (the old code returned the
/// condition unchanged too).
Val emitBranchHint(IntrinsicCallExprAST* expr, Emitter& emitter) {
    if (expr->args.size() != 1) {
        return argCountError(expr, "1 (condition)", emitter);
    }
    Val argVal = emitter.emit(expr->args[0]);
    if (!argVal.isValid()) return {};

    Val out;
    out.v = argVal.v;
    out.ty = expr->resolvedType;
    out.own = argVal.own;  // identity
    return out;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Dispatcher
// ─────────────────────────────────────────────────────────────────────────────

Val emitLucidIntrinsic(IntrinsicCallExprAST* expr,
                       const IntrinsicInfo& info,
                       Emitter& emitter) {
    if (!expr) return {};

    // ─── Special-case argument lowering ───────────────────────────────────
    //
    // These intrinsics do not lower their arguments uniformly. See the
    // file header for what makes each one special.
    switch (info.kind) {
        case IntrinsicKind::ScopeExit:  return emitScopeExit(expr, emitter);
        case IntrinsicKind::Sizeof:     return emitSizeof(expr, emitter);
        case IntrinsicKind::Alignof:    return emitAlignof(expr, emitter);
        case IntrinsicKind::Typeof:     return emitTypeof(expr, emitter);
        case IntrinsicKind::Nameof:     return emitNameof(expr, emitter);
        case IntrinsicKind::Bitcast:    return emitBitcast(expr, emitter);
        case IntrinsicKind::Alloc:      return emitAlloc(expr, emitter);
        case IntrinsicKind::Addrof:     return emitAddrof(expr, emitter);
        case IntrinsicKind::Ptrstr:     return emitPtrstr(expr, emitter);
        case IntrinsicKind::ToRef:      return emitToRef(expr, emitter);
        case IntrinsicKind::Tostr:      return emitTostr(expr, emitter);

        // ─── General-path Lucid intrinsics ────────────────────────────────
        case IntrinsicKind::ToPtr:      return emitToPtr(expr, emitter);
        case IntrinsicKind::PtrOffset:  return emitPtrOffset(expr, emitter);
        case IntrinsicKind::PtrDiff:    return emitPtrDiff(expr, emitter);
        case IntrinsicKind::Free:       return emitFree(expr, emitter);

        case IntrinsicKind::StrLen:     return emitStrLen(expr, emitter);
        case IntrinsicKind::StrPtr:     return emitStrPtr(expr, emitter);
        case IntrinsicKind::StrFromPtr: return emitStrFromPtr(expr, emitter);
        case IntrinsicKind::StrConcat:  return emitStrConcat(expr, emitter);
        case IntrinsicKind::StrSlice:   return emitStrSlice(expr, emitter);
        case IntrinsicKind::StrEq:      return emitStrEq(expr, emitter);
        case IntrinsicKind::StrByteAt:  return emitStrByteAt(expr, emitter);

        case IntrinsicKind::Likely:
        case IntrinsicKind::Unlikely:   return emitBranchHint(expr, emitter);

        default:
            break;
    }

    // ─── Unreachable: every Lucid intrinsic has a case above ──────────────
    //
    // Reaching this point means the registry has a `Lucid` intrinsic the
    // dispatcher doesn't know about. That's a registry/dispatcher
    // mismatch, not a user-facing condition.
    return notImplementedError(
        expr,
        "no dispatch case in emitLucidIntrinsic() — registry/dispatcher mismatch",
        emitter);
}

} // namespace codegen