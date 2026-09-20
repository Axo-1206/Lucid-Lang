/// @file codegen/ownership/DropGlue.cpp
/// @brief Implementation of lazy aggregate drop and copy glue generation.
///
/// ─── Design ───────────────────────────────────────────────────────────────
/// A generated glue function is a small loop over the aggregate's fields.
/// For each field:
///
///   - Drop glue: GEP to the field, load it, call `Ownership::drop` with
///     the field's type.
///   - Copy glue: GEP to the source field, load it, call
///     `Ownership::intoOwned` to get an Owned copy, GEP to the destination
///     field, store the copy.
///
/// The recursion is inherent: an aggregate containing an aggregate
/// triggers glue generation for the inner aggregate, which triggers glue
/// for its fields, and so on. Cycles are impossible because Lucid structs
/// cannot contain themselves by value (self-reference is only allowed
/// through pointers, and pointers own nothing).
///
/// ─── Function-Typed Fields ────────────────────────────────────────────────
/// A field whose AST type is a `FuncTypeAST` is lowered by `Types::structType`
/// to the runtime shape of a function value, not to the function's signature
/// type. For an `fn`-shaped field that's `ptr`; for a `cls`-shaped field
/// that's `lucid.Closure`. Both shapes are 1-word or 2-word values and, if
/// the field is `cls`, owning a claim on its env.
///
/// `Ownership::drop` and `Ownership::intoOwned` classify the field by its
/// *AST* type (which is the `FuncTypeAST`), so a `cls` field correctly
/// retains and releases its env, and an `fn` field is correctly a no-op.
/// The glue code itself doesn't need to distinguish the two — it hands the
/// field's AST type to `Ownership` and lets the classifier decide.
///
/// ─── Naming ───────────────────────────────────────────────────────────────
/// The generated functions are named `__drop_<TypeName>` and
/// `__copy_<TypeName>` where `<TypeName>` is derived from the type's
/// mangled name. This makes them readable in IR dumps and makes it easy
/// to correlate a glue function with its type.
///
/// ─── Linkage ──────────────────────────────────────────────────────────────
/// Internal linkage. Glue functions are only called from the module that
/// generated them; they are not part of any ABI. Internal linkage lets
/// LLVM's optimiser inline them and lets dead-code elimination remove them
/// if the type is never used.

#include "DropGlue.hpp"
#include "Ownership.hpp"

#include "codegen/Program.hpp"
#include "codegen/Types.hpp"

#include "core/ast/ResourceKind.hpp"
#include "core/trace/Trace.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// The LLVM struct type and the AST declaration for an aggregate type.
/// Returns an empty `AggregateInfo` if the type is not an aggregate that
/// needs glue.
///
/// An aggregate for glue purposes is:
///   - a `NamedTypeAST` whose `resolvedDecl` is a `StructDeclAST`
///   - a `NullableTypeAST`/`FallibleTypeAST`/`CombinedTypeAST` whose inner
///     type owns a resource (the tagged slot needs glue to drop the inner
///     value when the tag indicates "present")
///   - a `Fixed` `ArrayTypeAST` whose element type owns a resource
///
/// Today, only `NamedTypeAST → StructDeclAST` is handled. The tagged-slot
/// cases are structurally possible but not yet supported because the
/// emitter doesn't lower them to storage-shaped values yet. The fixed-array
/// case is also not yet supported.
struct AggregateInfo {
    llvm::StructType* llvmType = nullptr;
    StructDeclAST* decl = nullptr;
    std::vector<FieldDeclAST*> fields;
};

AggregateInfo getAggregateInfo(Types& types, TypeAST* type) {
    AggregateInfo info;

    if (!type) return info;

    if (type->isa<NamedTypeAST>()) {
        NamedTypeAST* named = type->as<NamedTypeAST>();
        if (named->resolvedDecl
            && named->resolvedDecl->isa<StructDeclAST>()) {
            info.decl = named->resolvedDecl->as<StructDeclAST>();
            info.llvmType = types.structType(info.decl);
            info.fields.assign(info.decl->fields.begin(),
                                info.decl->fields.end());
            return info;
        }
    }

    // Other aggregate kinds are not handled yet. Returning an empty
    // `AggregateInfo` signals "not an aggregate" to the caller.
    return info;
}

std::string glueName(const char* prefix, TypeAST* type, Types& types) {
    return std::string("__") + prefix + "_" + types.typeName(type);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Drop Glue
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* generateDropGlue(Ownership& ownership,
                                  TypeAST* type,
                                  ProgramState& program) {
    // ─── Cache hit ────────────────────────────────────────────────────────
    if (llvm::Function* cached = program.lookupDropGlue(type)) {
        return cached;
    }

    // ─── Aggregate info ───────────────────────────────────────────────────
    AggregateInfo info = getAggregateInfo(program.types(), type);
    if (!info.llvmType || !info.decl) {
        // Not an aggregate that needs glue.
        return nullptr;
    }

    // ─── Function signature ───────────────────────────────────────────────
    llvm::LLVMContext& llvmCtx = program.llvmContext();
    llvm::Module& module = program.module();
    llvm::IRBuilder<>& builder = program.builder();

    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvmCtx), {ptrTy}, /*isVarArg=*/false);

    std::string name = glueName("drop", type, program.types());
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, name, module);
    fn->getArg(0)->setName("value");

    // ─── Body ─────────────────────────────────────────────────────────────
    // Save the caller's insertion point — glue generation happens during
    // some other function's lowering, and it must not disturb that.
    llvm::IRBuilderBase::InsertPointGuard guard(builder);

    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(llvmCtx, "entry", fn);
    builder.SetInsertPoint(entry);

    for (size_t i = 0; i < info.fields.size(); ++i) {
        FieldDeclAST* field = info.fields[i];
        if (!field || !field->type) continue;

        // Skip fields that own nothing. This is the common case for a
        // struct with mostly primitive fields and one resource field.
        ResourceKind fieldKind = classifyResourceKind(field->type);
        if (fieldKind == ResourceKind::None) continue;

        // GEP to the field.
        llvm::Value* fieldPtr = builder.CreateStructGEP(
            info.llvmType, fn->getArg(0), static_cast<unsigned>(i),
            "field_" + program.pool.lookup(field->name));

        // Load the field value.
        llvm::Type* fieldTy = info.llvmType->getElementType(i);
        llvm::Value* fieldVal = builder.CreateLoad(
            fieldTy, fieldPtr, "field_val_" + program.pool.lookup(field->name));

        // Drop it. `Ownership::drop` dispatches on the field's AST type,
        // which is the source of truth for the field's resource kind.
        ownership.drop(field->type, fieldVal, builder);
    }

    builder.CreateRetVoid();

    // ─── Cache and return ─────────────────────────────────────────────────
    program.storeDropGlue(type, fn);

    Trace::detail("Generated drop glue for '",
                  program.types().typeName(type), "'");

    return fn;
}

// ─────────────────────────────────────────────────────────────────────────────
// Copy Glue
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* generateCopyGlue(Ownership& ownership,
                                  TypeAST* type,
                                  ProgramState& program) {
    // ─── Cache hit ────────────────────────────────────────────────────────
    if (llvm::Function* cached = program.lookupCopyGlue(type)) {
        return cached;
    }

    // ─── Aggregate info ───────────────────────────────────────────────────
    AggregateInfo info = getAggregateInfo(program.types(), type);
    if (!info.llvmType || !info.decl) {
        return nullptr;
    }

    // ─── Function signature ───────────────────────────────────────────────
    // The copy function takes a pointer to the source aggregate and
    // returns a pointer to a freshly allocated copy.
    //
    // The return type is `ptr`, not the specific struct type, so the
    // function can be created before the struct body is set (which it
    // always is by the time glue is needed). Callers cast as needed.
    llvm::LLVMContext& llvmCtx = program.llvmContext();
    llvm::Module& module = program.module();
    llvm::IRBuilder<>& builder = program.builder();

    llvm::Type* ptrTy = llvm::PointerType::get(llvmCtx, 0);
    llvm::FunctionType* fnTy = llvm::FunctionType::get(
        ptrTy, {ptrTy}, /*isVarArg=*/false);

    std::string name = glueName("copy", type, program.types());
    llvm::Function* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, name, module);
    fn->getArg(0)->setName("src");

    // ─── Body ─────────────────────────────────────────────────────────────
    llvm::IRBuilderBase::InsertPointGuard guard(builder);

    llvm::BasicBlock* entry =
        llvm::BasicBlock::Create(llvmCtx, "entry", fn);
    builder.SetInsertPoint(entry);

    // Allocate a fresh aggregate via `__lucid_alloc`.
    uint64_t size = program.types().sizeOf(type);
    llvm::Value* sizeConst =
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(llvmCtx), size);
    llvm::Function* allocFn =
        program.abi().declareOrGet(RuntimeFn::Alloc);
    llvm::Value* dstRaw = builder.CreateCall(allocFn, {sizeConst}, "copy_alloc");

    // Cast the raw allocation to a pointer to the aggregate's struct type.
    // With opaque pointers, the cast is a no-op at the LLVM level, but
    // naming the type here keeps the generated IR self-documenting and
    // lets the GEPs below use the struct type directly.
    //
    // `llvm::PointerType::get(llvmCtx, 0)` is the opaque-pointer form.
    // `PointerType::getUnqual(...)` is the pre-opaque-pointer API; it
    // exists as a deprecated shim in LLVM 17+ and emits warnings under
    // `-Wdeprecated-declarations`.
    llvm::Type* structPtrTy = llvm::PointerType::get(llvmCtx, 0);
    llvm::Value* dst = builder.CreatePointerCast(
        dstRaw, structPtrTy, "copy_dst");

    // Copy each field.
    for (size_t i = 0; i < info.fields.size(); ++i) {
        FieldDeclAST* field = info.fields[i];
        if (!field || !field->type) continue;

        llvm::Type* fieldTy = info.llvmType->getElementType(i);

        // GEP to source and destination field slots.
        llvm::Value* srcPtr = builder.CreateStructGEP(
            info.llvmType, fn->getArg(0), static_cast<unsigned>(i),
            "src_field_" + program.pool.lookup(field->name));
        llvm::Value* dstPtr = builder.CreateStructGEP(
            info.llvmType, dst, static_cast<unsigned>(i),
            "dst_field_" + program.pool.lookup(field->name));

        // Load the source field.
        llvm::Value* srcVal = builder.CreateLoad(
            fieldTy, srcPtr,
            "copy_src_val_" + program.pool.lookup(field->name));

        // Wrap as a Val and let `Ownership::intoOwned` produce a fresh
        // Owned copy. This handles all resource kinds uniformly:
        //   - None: bit copy.
        //   - Refcounted: retain.
        //   - OwnedBuffer: deep copy.
        //   - Aggregate: recursive copy glue.
        //   - Arena/Handle: Sema forbids; no-op.
        Val src{srcVal, field->type, Own::Borrowed};
        Val copied = ownership.intoOwned(src, builder);

        // Store the copy.
        builder.CreateStore(copied.v, dstPtr);
    }

    // Return the destination pointer.
    builder.CreateRet(dst);

    // ─── Cache and return ─────────────────────────────────────────────────
    program.storeCopyGlue(type, fn);

    Trace::detail("Generated copy glue for '",
                  program.types().typeName(type), "'");

    return fn;
}

} // namespace codegen