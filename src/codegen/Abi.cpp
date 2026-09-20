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
/// Inside each generated method, the row's tags (`I64`, `Str`, ...) are
/// expanded into `llvm::Type*` values via `llvmTypeForTag`. That function
/// is the single place where "what LLVM type does `Str` mean" is answered.
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
// The tags used in `functions.def` (`I1`, `I8`, ..., `Str`, `Arena`, `Desc`,
// `Void`) are a small vocabulary. This is the single place they're mapped
// to LLVM types.
//
// `ProgramState&` is needed because `Str`, `Slice`, `Arena`, and `Desc`
// name LLVM struct types that `Types` owns. Everything else can be
// constructed directly from `llvm::LLVMContext&`.
//
// The mapping is:
//
//   Void     — no LLVM value (used only for the return type)
//   I1       — i1
//   I8       — i8
//   I32      — i32
//   I64      — i64
//   F64      — double
//   Ptr      — opaque `ptr`
//   Str      — `lucid.String`
//   Slice    — `lucid.Slice`
//   Arena    — `lucid.Arena`
//   Desc     — `lucid.ArenaDescriptor`

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
    Str,
    Slice,
    Arena,
    Desc,
};

/// @brief Map an `AbiTag` to an `llvm::Type*`.
///
/// `Void` returns `nullptr` — the return-type path handles it specially
/// because `llvm::Type::getVoidTy` is a distinct LLVM type, not a null
/// pointer.
llvm::Type* llvmTypeForTag(AbiTag tag,
                           llvm::LLVMContext& llvmCtx,
                           ProgramState& program) {
    switch (tag) {
        case AbiTag::Void:  return nullptr;
        case AbiTag::I1:    return llvm::Type::getInt1Ty(llvmCtx);
        case AbiTag::I8:    return llvm::Type::getInt8Ty(llvmCtx);
        case AbiTag::I32:   return llvm::Type::getInt32Ty(llvmCtx);
        case AbiTag::I64:   return llvm::Type::getInt64Ty(llvmCtx);
        case AbiTag::F64:   return llvm::Type::getDoubleTy(llvmCtx);
        case AbiTag::Ptr:   return llvm::PointerType::get(llvmCtx, 0);
        case AbiTag::Str:   return program.types().stringType();
        case AbiTag::Slice: return program.types().sliceType();
        case AbiTag::Arena: return program.types().arenaType();
        case AbiTag::Desc:  return program.types().arenaDescriptorType();
    }
    return nullptr;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Tuple unpacking
// ─────────────────────────────────────────────────────────────────────────────
//
// A row's params are a parenthesised tuple: `(Ptr, Str, I64)`. Writing the
// macro name directly before the tuple — `LUCID_RT_UNPACK Params` — makes it
// a normal invocation whose `__VA_ARGS__` is the bare list `Ptr, Str, I64`.
// The empty tuple `()` gives an empty list. No counting is involved, so
// there is no arity cap and no empty-argument special case.

#define LUCID_RT_UNPACK(...) __VA_ARGS__

// ─────────────────────────────────────────────────────────────────────────────
// The RuntimeFn → (symbol, signature) table
// ─────────────────────────────────────────────────────────────────────────────
//
// This is the second reader of `functions.def`. It builds a static table
// that maps each enumerator to its symbol name and the list of `AbiTag`s
// for its parameters and return type.

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

#define Void  AbiTag::Void
#define I1    AbiTag::I1
#define I8    AbiTag::I8
#define I32   AbiTag::I32
#define I64   AbiTag::I64
#define F64   AbiTag::F64
#define Ptr   AbiTag::Ptr
#define Str   AbiTag::Str
#define Slice AbiTag::Slice
#define Arena AbiTag::Arena
#define Desc  AbiTag::Desc

// Build the static table.
#define LUCID_RT(EnumName, Symbol, Ret, Params)                               \
    { RuntimeFn::EnumName,                                                    \
      RuntimeFnInfo{ Symbol, Ret,                                             \
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
#undef Str
#undef Slice
#undef Arena
#undef Desc

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
        : llvmTypeForTag(info.returnTag, llvmCtx, program_);

    // Same by-pointer rule as the parameter check above: a struct returned
    // by value would be subject to the same mismatch. The formatters use
    // an out-pointer (a `Ptr` in the first parameter position) rather than
    // returning by value.
    assert(info.returnTag != AbiTag::Str && info.returnTag != AbiTag::Slice &&
           info.returnTag != AbiTag::Arena && info.returnTag != AbiTag::Desc &&
           "runtime function returns a struct by value; use an out-pointer "
           "instead — see the by-pointer convention in "
           "runtime-abi/functions.def");

    assert(returnType && "runtime function return type is not Void but "
                         "no LLVM type was produced for it");

    // ─── Parameter types ──────────────────────────────────────────────────
    std::vector<llvm::Type*> paramTypes;
    paramTypes.reserve(info.paramTags.size());
    for (AbiTag tag : info.paramTags) {
        // ─── Enforce the by-pointer convention ────────────────────────────
        // See the "By-Pointer Convention" section of functions.def.
        // Passing a struct by value across the runtime boundary disagrees
        // with the platform C ABI on any target Lucid supports, and the
        // disagreement is silent until the call runs. The convention is
        // that struct types are always passed by pointer (the `Ptr` tag),
        // so a `Str`/`Slice`/`Arena`/`Desc` tag in a parameter position is
        // a table error.
        assert(tag != AbiTag::Str && tag != AbiTag::Slice &&
               tag != AbiTag::Arena && tag != AbiTag::Desc &&
               "runtime function parameter uses a by-value struct tag; "
               "pass a Ptr to the struct instead — see the by-pointer "
               "convention in runtime-abi/functions.def");

        llvm::Type* paramType = llvmTypeForTag(tag, llvmCtx, program_);
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