/// @file LucidIntrinsicEmitter.cpp
/// @brief Implementation of Lucid-specific intrinsic emissions.

#include "LucidIntrinsicEmitter.hpp"
#include "../CodeGenType.hpp"
#include "codegen/runtime/closure/CodeGenClosure.hpp"
#include "../support/CodeGenAlloca.hpp"
#include "../support/CodeGenPanic.hpp"
#include "../support/LLVMHelpers.hpp"
#include "codegen/CodeGen.hpp"

#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/DerivedTypes.h>

#include <unordered_set>
#include <cassert>

namespace codegen {

// ─── Helper: Get type name as string ────────────────────────────────────

static std::string getLucidTypeName(CodeGenContext& ctx, TypeAST* type) {
    if (!type) return "unknown";
    return getTypeName(ctx, type);
}

// ─── Helper: concatenate two strings via the runtime ─────────────────────

static llvm::Value* emitStrConcat(llvm::Value* a, llvm::Value* b, CodeGenContext& ctx) {
    llvm::Function* concatFunc = ctx.getRuntimeFn(RuntimeFn::StrConcat);
    return ctx.builder.CreateCall(concatFunc, {a, b});
}

// ─── Helper: Format an integer as a string ─────────────────────────────

static llvm::Value* emitIntToStr(llvm::Value* val, PrimitiveKind kind, CodeGenContext& ctx) {
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);
    
    // Extend or truncate to i64 for the runtime function
    llvm::Value* intVal = val;
    if (intVal->getType() != i64) {
        if (isSignedIntegerKind(kind)) {
            intVal = ctx.builder.CreateSExtOrTrunc(intVal, i64);
        } else {
            intVal = ctx.builder.CreateZExtOrTrunc(intVal, i64);
        }
    }
    
    // Use signed or unsigned formatter based on the kind
    if (isSignedIntegerKind(kind)) {
        llvm::Function* fn = ctx.getRuntimeFn(RuntimeFn::IntToStr);
        return ctx.builder.CreateCall(fn, {intVal});
    } else {
        llvm::Function* fn = ctx.getRuntimeFn(RuntimeFn::UintToStr);
        return ctx.builder.CreateCall(fn, {intVal});
    }
}

// ─── #tostr core: recursive value formatter ───────────────────────────────
//
// Shared by the top-level #tostr(x) call and by struct-field formatting,
// which needs to recurse into each field's own type. sourceExpr is the
// syntactic expression this value came from - only meaningful for the
// function/closure case (to look up a declared name), and only available
// for the top-level call; recursive calls into struct fields pass nullptr
// since a field's value has no source expression of its own once read out
// of the struct.
static llvm::Value* emitTostrValue(
    llvm::Value* val,
    TypeAST* type,
    ExprAST* sourceExpr,
    SourceLocation loc,
    CodeGenContext& ctx
) {
    llvm::Type* strType = ctx.getStringType();
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);

    // ─── Functions/closures: declared name ────────────────────────────────
    if (type && type->isa<FuncTypeAST>()) {
        std::string nameStr;
        IdentifierExprAST* ident = sourceExpr ? sourceExpr->as<IdentifierExprAST>() : nullptr;
        FieldAccessExprAST* field = (!ident && sourceExpr) ? sourceExpr->as<FieldAccessExprAST>() : nullptr;
        if (ident) {
            nameStr = ctx.pool.lookup(ident->name);
        } else if (field) {
            nameStr = ctx.pool.lookup(field->fieldName);
        } else {
            // No source expression (recursive struct-field call) or an
            // anonymous closure literal with no declared name to report.
            nameStr = "<closure>";
        }
        return ctx.createStringLiteral(nameStr);
    }

    // ─── Primitives: format by value ──────────────────────────────────────
    if (type && type->isa<PrimitiveTypeAST>()) {
        PrimitiveKind kind = type->as<PrimitiveTypeAST>()->primitiveKind;

        // ─── String: identity ──────────────────────────────────────────────
        if (kind == PrimitiveKind::String) {
            return val;
        }

        // ─── Bool ──────────────────────────────────────────────────────────
        if (kind == PrimitiveKind::Bool) {
            llvm::Function* fn = ctx.getRuntimeFn(RuntimeFn::BoolToStr);
            return ctx.builder.CreateCall(fn, {val});
        }

        // ─── Char ──────────────────────────────────────────────────────────
        if (kind == PrimitiveKind::Char) {
            llvm::Type* i32 = llvm::Type::getInt32Ty(ctx.llvmCtx);
            llvm::Function* fn = ctx.getRuntimeFn(RuntimeFn::CharToStr);
            llvm::Value* charVal = val;
            if (charVal->getType() != i32) {
                charVal = ctx.builder.CreateZExtOrTrunc(charVal, i32);
            }
            return ctx.builder.CreateCall(fn, {charVal});
        }

        // ─── Integer (signed or unsigned) ──────────────────────────────────
        if (isIntegerKind(kind)) {
            return emitIntToStr(val, kind, ctx);
        }

        // ─── Floating point ─────────────────────────────────────────────────
        if (isFloatKind(kind)) {
            // NOTE: Decimal is 128-bit high-precision. Routing it
            // through the same double formatter as Float/Double loses
            // precision - a real fix needs its own
            // __lucid_decimal_to_str helper. Known, scoped-out gap
            // for Decimal specifically.
            llvm::Type* f64 = llvm::Type::getDoubleTy(ctx.llvmCtx);
            llvm::Function* fn = ctx.getRuntimeFn(RuntimeFn::FloatToStr);
            llvm::Value* floatVal = val;
            if (floatVal->getType() != f64) {
                floatVal = ctx.builder.CreateFPExt(floatVal, f64);
            }
            return ctx.builder.CreateCall(fn, {floatVal});
        }

        // ─── Fallback for unknown primitive ──────────────────────────────
        return ctx.createStringLiteral("<unknown primitive>");
    }

    // ─── Named types: enum or struct ──────────────────────────────────────
    if (type && type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();

        // ─── Enum: "EnumType.VariantName" via a runtime switch ─────────────
        if (named->resolvedDecl && named->resolvedDecl->isa<EnumDeclAST>()) {
            EnumDeclAST* enumDecl = named->resolvedDecl->as<EnumDeclAST>();
            std::string enumName = ctx.pool.lookup(enumDecl->name);

            llvm::Function* func = ctx.getCurrentFunction();
            llvm::BasicBlock* defaultBlock = llvm::BasicBlock::Create(
                ctx.llvmCtx, "tostr_enum_unknown", func);
            llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(
                ctx.llvmCtx, "tostr_enum_merge", func);

            llvm::SwitchInst* sw = ctx.builder.CreateSwitch(
                val, defaultBlock, static_cast<unsigned>(enumDecl->variants.size()));

            std::vector<std::pair<llvm::BasicBlock*, llvm::Value*>> incoming;

            for (size_t i = 0; i < enumDecl->variants.size(); ++i) {
                EnumVariantAST* variant = enumDecl->variants[i];
                llvm::ConstantInt* constVal = enumDecl->constantForVariant(variant->name);
                if (!constVal) continue;

                llvm::BasicBlock* caseBlock = llvm::BasicBlock::Create(
                    ctx.llvmCtx, "tostr_enum_" + ctx.pool.lookup(variant->name), func);
                sw->addCase(constVal, caseBlock);

                ctx.builder.SetInsertPoint(caseBlock);
                std::string label = enumName + "." + ctx.pool.lookup(variant->name);
                incoming.push_back({caseBlock, ctx.createStringLiteral(label)});
                ctx.builder.CreateBr(mergeBlock);
            }

            // ─── Default: value matches no known variant ───────────────────
            ctx.builder.SetInsertPoint(defaultBlock);
            llvm::Value* asI64 = ctx.builder.CreateSExtOrTrunc(val, i64);
            llvm::Function* intFn = ctx.getRuntimeFn(RuntimeFn::IntToStr);
            llvm::Value* defaultStr = ctx.builder.CreateCall(intFn, {asI64});
            incoming.push_back({defaultBlock, defaultStr});
            ctx.builder.CreateBr(mergeBlock);

            ctx.builder.SetInsertPoint(mergeBlock);
            llvm::PHINode* phi = ctx.builder.CreatePHI(
                strType, static_cast<unsigned>(incoming.size()), "tostr_enum_result");
            for (auto& pair : incoming) {
                phi->addIncoming(pair.second, pair.first);
            }
            return phi;
        }

        // ─── Struct: "Name{ field: value, ... }", or the `str` override ────
        if (named->resolvedDecl && named->resolvedDecl->isa<StructDeclAST>()) {
            StructDeclAST* structDecl = named->resolvedDecl->as<StructDeclAST>();
            std::string structName = ctx.pool.lookup(structDecl->name);

            llvm::StructType* llvmStructType = ctx.lookupStruct(structDecl);
            if (!llvmStructType) {
                ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                        "struct '", structName, "' has no LLVM type");
                return llvm::Constant::getNullValue(strType);
            }

            // Reads a field out of `val`, which may be a pointer to the
            // struct (GEP + load) or an already-loaded aggregate value
            // (ExtractValue) - the same dual path lowerFieldAccessExpr
            // (CodeGenExpr.cpp) already has to handle for the same reason.
            auto readField = [&](size_t index) -> llvm::Value* {
                if (val->getType()->isPointerTy()) {
                    llvm::Type* fieldType = llvmStructType->getElementType(index);
                    std::vector<llvm::Value*> indices = {
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx), 0),
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx.llvmCtx),
                                                static_cast<uint32_t>(index))
                    };
                    llvm::Value* fieldPtr = ctx.builder.CreateInBoundsGEP(
                        llvmStructType, val, indices, "tostr_field_ptr");
                    return ctx.builder.CreateLoad(fieldType, fieldPtr, "tostr_field_load");
                }
                return ctx.builder.CreateExtractValue(
                    val, static_cast<unsigned>(index), "tostr_field_val");
            };

            // ─── Custom `str` field override ──────────────────────────────
            InternedString strFieldName = ctx.pool.intern("str");
            size_t strIndex = structDecl->indexOfField(strFieldName);
            if (strIndex != SIZE_MAX) {
                FieldDeclAST* strField = structDecl->fields[strIndex];
                bool isValidOverride = strField->type && strField->type->isa<FuncTypeAST>();
                if (isValidOverride) {
                    FuncTypeAST* fnType = strField->type->as<FuncTypeAST>();
                    isValidOverride = fnType->params.size() == 0 &&
                        fnType->returnType &&
                        fnType->returnType->isa<PrimitiveTypeAST>() &&
                        fnType->returnType->as<PrimitiveTypeAST>()->primitiveKind == PrimitiveKind::String;
                }
                if (isValidOverride) {
                    llvm::Value* closureVal = readField(strIndex);
                    llvm::Value* funcPtr = ctx.builder.CreateExtractValue(
                        closureVal, 0, "str_override_func");
                    llvm::Value* envPtr = ctx.builder.CreateExtractValue(
                        closureVal, 1, "str_override_env");
                    return emitClosureCall(funcPtr, envPtr, {}, strType, ctx);
                }
            }

            // ─── No override: synthesize "Name{ f1: v1, f2: v2 }" ──────────
            if (structDecl->fields.empty()) {
                return ctx.createStringLiteral(structName + "{}");
            }

            llvm::Value* result = ctx.createStringLiteral(structName + "{ ");
            for (size_t i = 0; i < structDecl->fields.size(); ++i) {
                FieldDeclAST* field = structDecl->fields[i];
                std::string fieldPrefix = ctx.pool.lookup(field->name) + ": ";

                llvm::Value* fieldVal = readField(i);
                llvm::Value* fieldStr = emitTostrValue(fieldVal, field->type, nullptr, loc, ctx);
                if (!fieldStr) {
                    fieldStr = llvm::Constant::getNullValue(strType);
                }

                result = emitStrConcat(result, ctx.createStringLiteral(fieldPrefix), ctx);
                result = emitStrConcat(result, fieldStr, ctx);
                if (i + 1 < structDecl->fields.size()) {
                    result = emitStrConcat(result, ctx.createStringLiteral(", "), ctx);
                }
            }
            result = emitStrConcat(result, ctx.createStringLiteral(" }"), ctx);
            return result;
        }
    }

    // ─── Trait, generic params, or anything else not yet covered ─────────
    // Trait-by-value is mechanically identical to the struct case above
    // (walk TraitDeclAST::fields instead of StructDeclAST::fields, no
    // `str` override since traits have no methods) but isn't wired up
    // yet. Trait-by-reference (&TraitA) needs the field-offset-table
    // design agreed on separately and isn't implemented at all yet.
    assert(false && "#tostr is not implemented for this type yet");
    return llvm::Constant::getNullValue(strType);
}


// ─── Type Inspection Intrinsics ──────────────────────────────────────────

llvm::Value* emitLucidTypeIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);
    llvm::Type* i8Ptr = llvm::PointerType::get(ctx.llvmCtx, 0);

    // ─── #sizeof(T) ──────────────────────────────────────────────────────
    if (kind == IntrinsicKind::Sizeof) {
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
    if (kind == IntrinsicKind::Alignof) {
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
    if (kind == IntrinsicKind::Bitcast) {
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
    if (kind == IntrinsicKind::Typeof) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#typeof' requires an argument");
            return nullptr;
        }

        TypeAST* type = expr->args[0]->resolvedType;
        if (!type) {
            ctx.diagnostics.errorAt(DiagCode::Sem_TypeMismatch, loc,
                                   "could not determine type for '#typeof'");
            return nullptr;
        }

        std::string typeName = getLucidTypeName(ctx, type);
        return ctx.createStringLiteral(typeName);
    }

    // ─── #nameof(x) ──────────────────────────────────────────────────────
    if (kind == IntrinsicKind::Nameof) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#nameof' requires an argument");
            return nullptr;
        }

        std::string nameStr;
        ExprAST* arg = expr->args[0];
        if (arg->isa<IdentifierExprAST>()) {
            nameStr = ctx.pool.lookup(arg->as<IdentifierExprAST>()->name);
        } if (arg->isa<FieldAccessExprAST>()) {
            nameStr = ctx.pool.lookup(arg->as<FieldAccessExprAST>()->fieldName);
        } else {
            nameStr = "unknown";
        }

        return ctx.createStringLiteral(nameStr);
    }

    // ─── #tostr(x) ──────────────────────────────────────────────────────
    if (kind == IntrinsicKind::Tostr) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#tostr' requires an argument");
            return nullptr;
        }

        return emitTostrValue(args[0], expr->args[0]->resolvedType, expr->args[0], loc, ctx);
    }

    // ─── #ptrstr(x) ──────────────────────────────────────────────────────
    if (kind == IntrinsicKind::Ptrstr) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#ptrstr' requires an argument");
            return nullptr;
        }

        // args[0] is the raw, un-loaded address (see the Ptrstr
        // special-case in emitIntrinsicFromAST, IntrinsicEmitter.cpp) -
        // it must not be loaded, since the whole point is reporting the
        // address itself, not the value stored there.
        llvm::Value* addr = args[0];
        if (addr->getType() != i8Ptr) {
            addr = ctx.builder.CreateBitCast(addr, i8Ptr);
        }

        llvm::Function* fn = ctx.getRuntimeFn(RuntimeFn::PtrToHexString);

        return ctx.builder.CreateCall(fn, {addr});
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown type intrinsic '#", name, "'");
    return nullptr;
}

// ─── Pointer Intrinsics ──────────────────────────────────────────────────

llvm::Value* emitLucidPointerIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── toPtr(ref) ──────────────────────────────────────────────────────
    if (kind == IntrinsicKind::ToPtr) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#toPtr' requires an argument");
            return nullptr;
        }
        return args[0];
    }

    // ─── ptrOffset(ptr, n) ──────────────────────────────────────────────
    if (kind == IntrinsicKind::PtrOffset) {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#ptrOffset' requires 2 arguments");
            return nullptr;
        }

        llvm::Value* ptr = args[0];
        llvm::Value* offset = args[1];

        // ctx.getPointeeType() is only a stub returning i8 (LLVM's opaque
        // pointers carry no element type once lowered). The real pointee
        // type still exists on the Lucid side as PtrTypeAST::inner - use
        // that so offsets are in units of T, not raw bytes.
        llvm::Type* elemType = llvm::Type::getInt8Ty(ctx.llvmCtx);
        if (expr && !expr->args.empty() && expr->args[0]->resolvedType &&
            expr->args[0]->resolvedType->isa<PtrTypeAST>()) {
            TypeAST* pointee = expr->args[0]->resolvedType->as<PtrTypeAST>()->inner;
            if (llvm::Type* resolvedElem = getType(ctx, pointee)) {
                elemType = resolvedElem;
            }
        }

        llvm::Value* gep = ctx.builder.CreateInBoundsGEP(
            elemType,
            ptr,
            offset,
            "ptr_offset"
        );

        return gep;
    }

    // ─── ptrDiff(p1, p2) ──────────────────────────────────────────────
    if (kind == IntrinsicKind::PtrDiff) {
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

        // Same underlying issue as ptrOffset: ctx.getPointeeType() is a
        // stub that always reports i8/size-1, so recover the real element
        // size from the Lucid-level pointee type (PtrTypeAST::inner).
        uint64_t elemSize = 1;
        if (expr && !expr->args.empty() && expr->args[0]->resolvedType &&
            expr->args[0]->resolvedType->isa<PtrTypeAST>()) {
            TypeAST* pointee = expr->args[0]->resolvedType->as<PtrTypeAST>()->inner;
            uint64_t resolvedSize = getTypeSize(ctx, pointee);
            if (resolvedSize > 0) elemSize = resolvedSize;
        }

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
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);
    llvm::Type* i8Ptr = llvm::PointerType::get(ctx.llvmCtx, 0);

    // ─── #alloc(T, count) -> *T ──────────────────────────────────────
    // NOTE: T is a type argument, not a value argument - like #sizeof(T),
    // #bitcast(T,x), and #simd_splat(x), the element type comes from the
    // call's resolved type (*T), and `args` holds only the value arg(s)
    // (count). The previous version indexed args[1] as if T occupied a
    // value-arg slot, and never multiplied by sizeof(T) - it passed the
    // raw count straight through as a byte size.
    if (kind == IntrinsicKind::Alloc) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#alloc' requires an argument (count)");
            return nullptr;
        }

        llvm::Type* targetType = getType(ctx, expr->resolvedType);

        uint64_t elemSize = 1;
        if (expr->resolvedType && expr->resolvedType->isa<PtrTypeAST>()) {
            TypeAST* pointee = expr->resolvedType->as<PtrTypeAST>()->inner;
            uint64_t resolvedSize = getTypeSize(ctx, pointee);
            if (resolvedSize > 0) elemSize = resolvedSize;
        }

        llvm::Value* count = args[0];
        if (count->getType() != i64) {
            count = ctx.builder.CreateIntCast(count, i64, false, "alloc_count");
        }
        llvm::Value* size = ctx.builder.CreateMul(
            count,
            llvm::ConstantInt::get(i64, elemSize),
            "alloc_size"
        );

        llvm::Function* allocFunc = ctx.getRuntimeFn(RuntimeFn::Alloc);

        llvm::Value* result = ctx.builder.CreateCall(allocFunc, {size});

        if (targetType && targetType->isPointerTy()) {
            return ctx.builder.CreateBitCast(result, targetType);
        }
        return result;
    }

    // ─── #free(ptr) ──────────────────────────────────────────────────────
    if (kind == IntrinsicKind::Free) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#free' requires an argument");
            return nullptr;
        }

        llvm::Function* freeFunc = ctx.getRuntimeFn(RuntimeFn::Free);

        llvm::Value* ptr = args[0];
        if (ptr->getType() != i8Ptr) {
            ptr = ctx.builder.CreateBitCast(ptr, i8Ptr);
        }
        ctx.builder.CreateCall(freeFunc, {ptr});
        return nullptr;
    }

    ctx.diagnostics.errorAt(DiagCode::Sem_UnknownIntrinsic, loc,
                            "unknown memory management intrinsic '#", name, "'");
    return nullptr;
}

// ─── String Intrinsics ────────────────────────────────────────────────────

llvm::Value* emitLucidStringIntrinsic(
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    llvm::Type* strType = ctx.getStringType();
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx.llvmCtx);

    // ─── str_len(s) ──────────────────────────────────────────────────────
    if (kind == IntrinsicKind::StrLen) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_len' requires a string argument");
            return nullptr;
        }

        llvm::Value* str = args[0];
        return ctx.builder.CreateExtractValue(str, 1, "str_len");
    }

    // ─── str_ptr(s) ──────────────────────────────────────────────────────
    if (kind == IntrinsicKind::StrPtr) {
        if (args.empty()) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_ptr' requires a string argument");
            return nullptr;
        }

        llvm::Value* str = args[0];
        return ctx.builder.CreateExtractValue(str, 0, "str_ptr");
    }

    // ─── str_from_ptr(ptr, len) ──────────────────────────────────────────
    if (kind == IntrinsicKind::StrFromPtr) {
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
    if (kind == IntrinsicKind::StrConcat) {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_concat' requires 2 arguments");
            return nullptr;
        }

        llvm::Function* concatFunc = ctx.getRuntimeFn(RuntimeFn::StrConcat);

        return ctx.builder.CreateCall(concatFunc, {args[0], args[1]});
    }

    // ─── str_slice(s, from, to) ──────────────────────────────────────────
    if (kind == IntrinsicKind::StrSlice) {
        if (args.size() < 3) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_slice' requires 3 arguments");
            return nullptr;
        }

        llvm::Function* sliceFunc = ctx.getRuntimeFn(RuntimeFn::StrSlice);

        return ctx.builder.CreateCall(sliceFunc, {args[0], args[1], args[2]});
    }

    // ─── str_eq(a, b) ─────────────────────────────────────────────────────
    if (kind == IntrinsicKind::StrEq) {
        if (args.size() < 2) {
            ctx.diagnostics.errorAt(DiagCode::Sem_ArgCountMismatch, loc,
                                   "intrinsic '#str_eq' requires 2 arguments");
            return nullptr;
        }

        llvm::Function* eqFunc = ctx.getRuntimeFn(RuntimeFn::StrEq);

        return ctx.builder.CreateCall(eqFunc, {args[0], args[1]});
    }

    // ─── str_byte_at(s, i) ──────────────────────────────────────────────
    if (kind == IntrinsicKind::StrByteAt) {
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
    IntrinsicKind kind,
    const std::string& name,
    const std::vector<llvm::Value*>& args,
    IntrinsicCallExprAST* expr,
    CodeGenContext& ctx
) {
    SourceLocation loc = expr ? expr->loc : SourceLocation();

    // ─── scope_exit ──────────────────────────────────────────────────────
    if (kind == IntrinsicKind::ScopeExit) {
        // scope_exit is handled in Sema and stored on BlockStmtAST.
        // emitScopeExitCallback (below) emits these callbacks from
        // lowerBlockStmt, in LIFO order, at each block's exit point.
        // No runtime code is generated at the call site itself.
        return nullptr;
    }

    // ─── likely / unlikely ──────────────────────────────────────────────
    if (kind == IntrinsicKind::Likely || kind == IntrinsicKind::Unlikely) {
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

// ─── Scope Exit Callback Emission ────────────────────────────────────────
//
// Relocated from CodeGenStmt.cpp: this is the codegen half of #scope_exit,
// so it belongs alongside emitLucidControlIntrinsic rather than in the
// generic statement-lowering file.

void emitScopeExitCallback(const ScopeExitRegistration* reg, CodeGenContext& ctx) {
    if (!reg) return;

    // ─── Plain function-reference callback ────────────────────────────────
    if (reg->callback) {
        llvm::Value* callback = ctx.lookupFunction(reg->callback);
        if (!callback) {
            callback = reg->callback->llvmFunction;
        }
        // Sema (validateScopeExit) guarantees a plain function-reference
        // callback resolves to a real declaration. If it didn't, that's a
        // Sema bug, not something CodeGen should diagnose at runtime.
        assert(callback && "scope_exit callback not found - Sema should have caught this");
        if (!callback) {
            return;
        }

        std::vector<llvm::Value*> args;
        for (ExprAST* arg : reg->args) {
            llvm::Value* argVal = lowerExpression(arg, ctx);
            if (!argVal) {
                return;
            }
            if (arg->isLValue) {
                llvm::Type* elemType = getType(ctx, arg->resolvedType);
                // Sema guarantees resolvedType is set
                assert(elemType && "Argument has no type in CodeGen");
                argVal = loadIfNeeded(argVal, elemType, ctx);
            }
            args.push_back(argVal);
        }

        llvm::Function* callee = llvm::dyn_cast<llvm::Function>(callback);
        assert(callee && "scope_exit callback value is not an llvm::Function");
        if (!callee) {
            return;
        }

        ctx.builder.CreateCall(callee, args);
        return;
    }

    // ─── Closure callback ──────────────────────────────────────────────────
    // reg->callback is null, meaning the argument wasn't a plain function
    // reference - it's a closure literal or a closure-typed expression.
    // reg->callExpr is the original #scope_exit(...) call; its first
    // argument is the callee slot, same convention used for its location
    // in diagnostics elsewhere in this function.
    assert(reg->callExpr && !reg->callExpr->args.empty() &&
           "scope_exit closure registration missing callee expression");
    if (!reg->callExpr || reg->callExpr->args.empty()) {
        return;
    }

    ExprAST* closureExpr = reg->callExpr->args[0];
    llvm::Value* closureVal = lowerExpression(closureExpr, ctx);
    if (!closureVal) {
        return;
    }
    if (closureExpr->isLValue) {
        llvm::Type* elemType = getType(ctx, closureExpr->resolvedType);
        assert(elemType && "Closure argument has no type in CodeGen");
        closureVal = loadIfNeeded(closureVal, elemType, ctx);
    }

    // Closure value is the { i8* func, i8* env } fat pointer built in
    // lowerClosure (CodeGenClosure.cpp) - unpack it for emitClosureCall.
    llvm::Value* funcPtr = ctx.builder.CreateExtractValue(
        closureVal, 0, "scope_exit_closure_func");
    llvm::Value* envPtr = ctx.builder.CreateExtractValue(
        closureVal, 1, "scope_exit_closure_env");

    std::vector<llvm::Value*> closureArgs;
    for (ExprAST* arg : reg->args) {
        llvm::Value* argVal = lowerExpression(arg, ctx);
        if (!argVal) {
            return;
        }
        if (arg->isLValue) {
            llvm::Type* elemType = getType(ctx, arg->resolvedType);
            assert(elemType && "Argument has no type in CodeGen");
            argVal = loadIfNeeded(argVal, elemType, ctx);
        }
        closureArgs.push_back(argVal);
    }

    // scope_exit callbacks are registered as a void intrinsic, so this
    // return type is always void - never a placeholder.
    emitClosureCall(funcPtr, envPtr, closureArgs, llvm::Type::getVoidTy(ctx.llvmCtx), ctx);
}

} // namespace codegen