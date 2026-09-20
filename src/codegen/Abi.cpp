/// @file codegen/Abi.cpp
/// @brief Implementation of the runtime ABI surface.
///
/// ─── How The X-Macro Expansion Works ──────────────────────────────────────
/// `Abi` reads `functions.def` in three places:
///
///   1. `Abi.hpp` — `LUCID_RT` emits the `RuntimeFn` enum rows.
///   2. `Abi.hpp` — `LUCID_RT` emits one variadic member template per row.
///   3. `Abi.cpp` — `LUCID_RT` emits the `RuntimeFn -> {symbol, tags}` table.
///
/// All three come from the same rows, so they can't drift. Nothing counts
/// parameters in the preprocessor: the table stores the tag list, and
/// `emitCall` checks each call against it.
///
/// ─── Type-Tag Mapping ─────────────────────────────────────────────────────
/// The row's tags (`I64`, `StrPtr`, ...) are expanded into `llvm::Type*`
/// values via `llvmTypeForTag`. That function is the single place where
/// "what LLVM type does `StrPtr` mean" is answered. Every pointer tag is the
/// opaque `ptr`; with opaque pointers LLVM cannot tell a `StrPtr` from a
/// `Ptr`, so the distinct pointer tags exist for the runtime's benefit
/// (runtime-abi/lucid_runtime.h gives each its own C++ pointee type).
///
/// ─── Attributes ───────────────────────────────────────────────────────────
/// `I1` is `i1 zeroext` on parameters and returns: that is what clang emits
/// for a C `bool`, and it is what makes an `i1` from generated code agree with
/// the runtime's one-byte `LucidBool`. `Panic` is `noreturn`. Both are set at
/// declaration time in `declareOrGet` and copied onto every call site by
/// `emitCall`.
///
/// ─── Usage Recording ──────────────────────────────────────────────────────
/// Every call to `declareOrGet` records the runtime function in
/// `program_.usedRuntimeFns()`. The set is read by the module pass to
/// populate the manifest's runtime-symbol list, which the interpreter and
/// AOT linker use to decide which runtime objects to link.
///
/// Recording happens in `declareOrGet`, not in the generated methods,
/// because every path into a runtime function goes through
/// `declareOrGet` — the generated methods call it, and `panicFn` and
/// `shutdownFn` call it. One hook covers every case.

#include "Abi.hpp"
#include "Program.hpp"
#include "runtime-abi/lucid_abi.h"

#include <llvm/IR/Attributes.h>
#include <llvm/IR/DerivedTypes.h>

#include <cassert>
#include <string>
#include <unordered_map>
#include <vector>

namespace codegen {

// ─────────────────────────────────────────────────────────────────────────────
// Type Tag Mapping
// ─────────────────────────────────────────────────────────────────────────────
//
// The tags used in `functions.def` are a small closed vocabulary. This is the
// single place they're mapped to LLVM types. Everything can be built from the
// `llvm::LLVMContext` alone; no struct type is ever needed, because structs
// cross the ABI by pointer.
//
//   Void      — no LLVM value (return position only)
//   I1        — i1   (zeroext; see `declareOrGet`)
//   I8        — i8
//   I32       — i32
//   I64       — i64
//   F64       — double
//   Ptr       — opaque `ptr`
//   StrPtr    — opaque `ptr`  (a lucid.String*)
//   SlicePtr  — opaque `ptr`  (a lucid.Slice*)
//   ArenaPtr  — opaque `ptr`  (a lucid.Arena*)
//   DescPtr   — opaque `ptr`  (a lucid.ArenaDescriptor*)

namespace {

/// @brief The tag vocabulary used by `functions.def`.
enum class AbiTag {
    Void,
    I1,
    I8,
    I32,
    I64,
    F64,
    Ptr,
    StrPtr,
    SlicePtr,
    ArenaPtr,
    DescPtr,
};

/// @brief Map an `AbiTag` to an `llvm::Type*`.
///
/// `Void` returns `nullptr` — the return-type path handles it specially
/// because `llvm::Type::getVoidTy` is a distinct LLVM type, not a null
/// pointer.
llvm::Type* llvmTypeForTag(AbiTag tag, llvm::LLVMContext& llvmCtx) {
    switch (tag) {
        case AbiTag::Void:     return nullptr;
        case AbiTag::I1:       return llvm::Type::getInt1Ty(llvmCtx);
        case AbiTag::I8:       return llvm::Type::getInt8Ty(llvmCtx);
        case AbiTag::I32:      return llvm::Type::getInt32Ty(llvmCtx);
        case AbiTag::I64:      return llvm::Type::getInt64Ty(llvmCtx);
        case AbiTag::F64:      return llvm::Type::getDoubleTy(llvmCtx);
        case AbiTag::Ptr:
        case AbiTag::StrPtr:
        case AbiTag::SlicePtr:
        case AbiTag::ArenaPtr:
        case AbiTag::DescPtr:  return llvm::PointerType::get(llvmCtx, 0);
    }
    return nullptr;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// Tuple unpacking
// ─────────────────────────────────────────────────────────────────────────────
//
// A row's params are a parenthesised tuple: `(Ptr, StrPtr, I64)`. Writing the
// macro name directly before the tuple — `LUCID_RT_UNPACK Params` — makes it
// a normal invocation whose `__VA_ARGS__` is the bare list `Ptr, StrPtr, I64`.
// The empty tuple `()` gives an empty list. No counting is involved, so
// there is no arity cap and no empty-argument special case.

#define LUCID_RT_UNPACK(...) __VA_ARGS__

// ─────────────────────────────────────────────────────────────────────────────
// The RuntimeFn → (symbol, signature) table
// ─────────────────────────────────────────────────────────────────────────────
//
// This is the second reader of `functions.def`. It builds a static table
// that maps each enumerator to its symbol name and the list of `AbiTag`s
// for its parameters and return type. The table's column 2 is a bare
// identifier, so `#Symbol` turns it into the string the LLVM declaration
// needs.

namespace {

struct RuntimeFnInfo {
    std::string_view symbol;
    AbiTag returnTag;
    std::vector<AbiTag> paramTags;
};

// ─── Tag-name → AbiTag conversion ─────────────────────────────────────────
//
// The `functions.def` rows contain bare tag identifiers (`I64`, `Ptr`, ...).
// Inside this file, we define each tag identifier as a macro that expands
// to the corresponding `AbiTag::TAG` value.

#define Void     AbiTag::Void
#define I1       AbiTag::I1
#define I8       AbiTag::I8
#define I32      AbiTag::I32
#define I64      AbiTag::I64
#define F64      AbiTag::F64
#define Ptr      AbiTag::Ptr
#define StrPtr   AbiTag::StrPtr
#define SlicePtr AbiTag::SlicePtr
#define ArenaPtr AbiTag::ArenaPtr
#define DescPtr  AbiTag::DescPtr

// Build the static table.
#define LUCID_RT(EnumName, Symbol, Ret, Params)                               \
    { RuntimeFn::EnumName,                                                    \
      RuntimeFnInfo{ #Symbol, Ret,                                            \
                     std::vector<AbiTag>{ LUCID_RT_UNPACK Params } } },

const std::unordered_map<RuntimeFn, RuntimeFnInfo>& runtimeFnTable() {
    static const std::unordered_map<RuntimeFn, RuntimeFnInfo> table = {
#include "runtime-abi/functions.def"
    };
    return table;
}

#undef LUCID_RT

#undef Void
#undef I1
#undef I8
#undef I32
#undef I64
#undef F64
#undef Ptr
#undef StrPtr
#undef SlicePtr
#undef ArenaPtr
#undef DescPtr

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

Abi::Abi(ProgramState& program)
    : program_(program)
{
}

// ─────────────────────────────────────────────────────────────────────────────
// Symbol Name Lookup
// ─────────────────────────────────────────────────────────────────────────────

std::string_view Abi::symbolName(RuntimeFn fn) const {
    const auto& table = runtimeFnTable();
    auto it = table.find(fn);
    if (it == table.end()) {
        assert(false && "RuntimeFn has no table row — this is a build error");
        return "<unknown runtime fn>";
    }
    return it->second.symbol;
}

// ─────────────────────────────────────────────────────────────────────────────
// Function Type Construction
// ─────────────────────────────────────────────────────────────────────────────

llvm::FunctionType* Abi::buildFunctionType(RuntimeFn fn) {
    const auto& table = runtimeFnTable();
    auto row = table.find(fn);
    assert(row != table.end() && "RuntimeFn has no table row");

    const RuntimeFnInfo& info = row->second;
    llvm::LLVMContext& llvmCtx = program_.module().getContext();

    // ─── Return type ──────────────────────────────────────────────────────
    llvm::Type* returnType = (info.returnTag == AbiTag::Void)
        ? llvm::Type::getVoidTy(llvmCtx)
        : llvmTypeForTag(info.returnTag, llvmCtx);
    assert(returnType && "runtime function return type is not Void but "
                         "no LLVM type was produced for it");

    // ─── Parameter types ──────────────────────────────────────────────────
    // There is no by-value struct tag, so nothing here can pass a struct by
    // value; the by-pointer convention is enforced by the tag vocabulary.
    std::vector<llvm::Type*> paramTypes;
    paramTypes.reserve(info.paramTags.size());
    for (AbiTag tag : info.paramTags) {
        llvm::Type* paramType = llvmTypeForTag(tag, llvmCtx);
        assert(paramType && "runtime function parameter has Void tag — "
                            "Void is only valid for the return position");
        paramTypes.push_back(paramType);
    }

    return llvm::FunctionType::get(returnType, paramTypes, /*isVarArg=*/false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Declaration
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* Abi::declareOrGet(RuntimeFn fn) {
    // Record the usage before the cache check, so a caller that
    // declares-but-doesn't-immediately-call still shows up in the
    // manifest's runtime symbol list.
    program_.usedRuntimeFns().insert(fn);

    // ─── Cache hit ────────────────────────────────────────────────────────
    auto cached = functionCache.find(fn);
    if (cached != functionCache.end()) {
        return cached->second;
    }

    // ─── Build the type, then declare ─────────────────────────────────────
    llvm::FunctionType* fnType = buildFunctionType(fn);
    std::string_view name = symbolName(fn);

    // `getOrInsertFunction` returns a `FunctionCallee`. With opaque pointers
    // it does NOT throw or bitcast when a function of this name already
    // exists with a different type — it hands back the existing function
    // paired with the type we asked for, and the mismatch goes unnoticed
    // until the linker or the JIT. So check it ourselves.
    llvm::FunctionCallee callee =
        program_.module().getOrInsertFunction(std::string(name), fnType);

    auto* llvmFn = llvm::dyn_cast<llvm::Function>(callee.getCallee());
    assert(llvmFn && "getOrInsertFunction returned a non-Function callee — "
                     "this means a non-function symbol has the same name");
    assert(llvmFn->getFunctionType() == fnType &&
           "runtime function already declared in this module with a "
           "different signature — two call sites disagree about the ABI");

    // Attributes belong at declaration time so they hold on every path.
    //
    // `i1 zeroext`: the C side passes and returns a whole byte (`LucidBool`),
    // and an unextended i1 leaves the rest of that byte undefined. This is
    // what clang emits for a C `bool`.
    const RuntimeFnInfo& info = runtimeFnTable().at(fn);
    if (info.returnTag == AbiTag::I1) {
        llvmFn->addRetAttr(llvm::Attribute::ZExt);
    }
    for (unsigned i = 0; i < info.paramTags.size(); ++i) {
        if (info.paramTags[i] == AbiTag::I1) {
            llvmFn->addParamAttr(i, llvm::Attribute::ZExt);
        }
    }
    if (fn == RuntimeFn::Panic) {
        llvmFn->addFnAttr(llvm::Attribute::NoReturn);
    }

    functionCache[fn] = llvmFn;
    return llvmFn;
}

// ─────────────────────────────────────────────────────────────────────────────
// Call Emission
// ─────────────────────────────────────────────────────────────────────────────

llvm::Value* Abi::emitCall(RuntimeFn fn,
                           llvm::IRBuilder<>& builder,
                           llvm::ArrayRef<llvm::Value*> args) {
    llvm::Function* llvmFn = declareOrGet(fn);

#ifndef NDEBUG
    llvm::FunctionType* fnType = llvmFn->getFunctionType();
    assert(args.size() == fnType->getNumParams() &&
           "wrong number of arguments for runtime function (see functions.def)");
    for (unsigned i = 0; i < args.size(); ++i) {
        assert(args[i] && args[i]->getType() == fnType->getParamType(i) &&
               "argument type does not match the runtime function's row "
               "in functions.def");
    }
#endif

    llvm::CallInst* call = builder.CreateCall(llvmFn, args);

    // Copy the declaration's attributes onto the call site, as clang does.
    // This is what makes a call to `__lucid_panic` itself `noreturn`, and it
    // keeps `zeroext` explicit where the backend lowers the call.
    call->setAttributes(llvmFn->getAttributes());

    return call->getType()->isVoidTy() ? nullptr : call;
}

// ─────────────────────────────────────────────────────────────────────────────
// Special Cases
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* Abi::panicFn() {
    // `noreturn` is applied in `declareOrGet`.
    return declareOrGet(RuntimeFn::Panic);
}

llvm::Function* Abi::shutdownFn() {
    return declareOrGet(RuntimeFn::Shutdown);
}

// ─────────────────────────────────────────────────────────────────────────────
// Cleanup
// ─────────────────────────────────────────────────────────────────────────────

#undef LUCID_RT_UNPACK

} // namespace codegen