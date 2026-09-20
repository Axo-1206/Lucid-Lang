/// @file codegen/Abi.cpp
/// @brief Implementation of the runtime ABI surface.
///
/// ─── How The X-Macro Expansion Works ──────────────────────────────────────
/// `functions.def` is included three times in the whole codebase:
///
///   1. `Abi.hpp` — defines `LUCID_RT` to emit the `RuntimeFn` enum rows.
///   2. `Abi.hpp` — defines `LUCID_RT` to emit the `Abi` method declarations.
///   3. `Abi.cpp` — defines `LUCID_RT` to emit the `Abi` method definitions.
///
/// The three expansions must stay in sync — if a row exists in one but not
/// the other two, the build breaks, which is the point. The table is the
/// source of truth and the three views can't drift.
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

// Turn a parenthesized tag tuple into a `std::vector<AbiTag>{...}`.
#define LUCID_RT_TAGS_0()             std::vector<AbiTag>{}
#define LUCID_RT_TAGS_1(A)            std::vector<AbiTag>{ A }
#define LUCID_RT_TAGS_2(A, B)         std::vector<AbiTag>{ A, B }
#define LUCID_RT_TAGS_3(A, B, C)      std::vector<AbiTag>{ A, B, C }

#define LUCID_RT_GET_MACRO(_1, _2, _3, NAME, ...) NAME
#define LUCID_RT_TAGS(Params) \
    LUCID_RT_GET_MACRO Params (LUCID_RT_TAGS_3, LUCID_RT_TAGS_2, \
                                LUCID_RT_TAGS_1, LUCID_RT_TAGS_0) Params

// Build the static table.
#define LUCID_RT(EnumName, Symbol, Ret, Params) \
    { RuntimeFn::EnumName, RuntimeFnInfo{ Symbol, Ret, LUCID_RT_TAGS(Params) } },

const std::unordered_map<RuntimeFn, RuntimeFnInfo>& runtimeFnTable() {
    static const std::unordered_map<RuntimeFn, RuntimeFnInfo> table = {
#include "runtime-abi/functions.def"
    };
    return table;
}

#undef LUCID_RT
#undef LUCID_RT_TAGS
#undef LUCID_RT_GET_MACRO
#undef LUCID_RT_TAGS_0
#undef LUCID_RT_TAGS_1
#undef LUCID_RT_TAGS_2
#undef LUCID_RT_TAGS_3

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
    auto it = typeCache.find(fn);
    if (it != typeCache.end()) {
        return it->second;
    }

    const auto& table = runtimeFnTable();
    auto row = table.find(fn);
    assert(row != table.end() && "RuntimeFn has no table row");

    const RuntimeFnInfo& info = row->second;
    llvm::LLVMContext& llvmCtx = program_.module().getContext();

    // ─── Return type ──────────────────────────────────────────────────────
    llvm::Type* returnType = (info.returnTag == AbiTag::Void)
        ? llvm::Type::getVoidTy(llvmCtx)
        : llvmTypeForTag(info.returnTag, llvmCtx, program_);

    assert(returnType && "runtime function return type is not Void but "
                         "no LLVM type was produced for it");

    // ─── Parameter types ──────────────────────────────────────────────────
    std::vector<llvm::Type*> paramTypes;
    paramTypes.reserve(info.paramTags.size());
    for (AbiTag tag : info.paramTags) {
        llvm::Type* paramType = llvmTypeForTag(tag, llvmCtx, program_);
        assert(paramType && "runtime function parameter has Void tag — "
                            "Void is only valid for the return position");
        paramTypes.push_back(paramType);
    }

    llvm::FunctionType* fnType =
        llvm::FunctionType::get(returnType, paramTypes, /*isVarArg=*/false);

    typeCache[fn] = fnType;
    return fnType;
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

    // `getOrInsertFunction` returns a `FunctionCallee`. If a function with
    // this name already exists in the module with a *different* type,
    // LLVM throws. That's a real bug — it means two call sites disagree
    // about the runtime ABI — and the throw is the right behavior.
    llvm::FunctionCallee callee =
        program_.module().getOrInsertFunction(std::string(name), fnType);

    llvm::Function* fn = llvm::dyn_cast<llvm::Function>(callee.getCallee());
    assert(fn && "getOrInsertFunction returned a non-Function callee — "
                 "this means a non-function symbol has the same name");

    functionCache[fn] = fn;
    return fn;
}

// ─────────────────────────────────────────────────────────────────────────────
// Special Cases
// ─────────────────────────────────────────────────────────────────────────────

llvm::Function* Abi::panicFn() {
    llvm::Function* fn = declareOrGet(RuntimeFn::Panic);

    // `__lucid_panic` never returns. The `noreturn` attribute is what tells
    // LLVM's optimiser (and, more importantly, the emitter) that no code
    // executes after the call.
    if (!fn->hasFnAttribute(llvm::Attribute::NoReturn)) {
        fn->addFnAttr(llvm::Attribute::NoReturn);
    }
    return fn;
}

llvm::Function* Abi::shutdownFn() {
    return declareOrGet(RuntimeFn::Shutdown);
}

// ─────────────────────────────────────────────────────────────────────────────
// Method Definitions (generated)
// ─────────────────────────────────────────────────────────────────────────────
//
// Each row in `functions.def` becomes one method body here. The method:
//   1. Declares the runtime function (or finds it cached). `declareOrGet`
//      also records the usage.
//   2. Collects the arguments into a `std::vector<llvm::Value*>`.
//   3. Emits the call through the caller-supplied `IRBuilder`.
//   4. Returns the call's result value, or `nullptr` for `void` returns.

// Helper: `Void_RETURNS_VALUE(call)` returns nullptr, everything else
// returns the call. This is how the X-macro table's return tag controls
// whether the method returns a value.

#define Void_RETURNS_VALUE(call)  (nullptr)
#define I1_RETURNS_VALUE(call)    (call)
#define I8_RETURNS_VALUE(call)    (call)
#define I32_RETURNS_VALUE(call)   (call)
#define I64_RETURNS_VALUE(call)   (call)
#define F64_RETURNS_VALUE(call)   (call)
#define Ptr_RETURNS_VALUE(call)   (call)
#define Str_RETURNS_VALUE(call)   (call)
#define Slice_RETURNS_VALUE(call) (call)
#define Arena_RETURNS_VALUE(call) (call)
#define Desc_RETURNS_VALUE(call)  (call)

// Turn a tuple of named parameters into a `std::vector<llvm::Value*>`.
//
// `LUCID_RT_ARG_LIST_0()`         → `{}`
// `LUCID_RT_ARG_LIST_1(A)`        → `{ _a0 }`
// `LUCID_RT_ARG_LIST_2(A, B)`     → `{ _a0, _a1 }`
// `LUCID_RT_ARG_LIST_3(A, B, C)`  → `{ _a0, _a1, _a2 }`
#define LUCID_RT_ARG_LIST_0()          std::vector<llvm::Value*>{}
#define LUCID_RT_ARG_LIST_1(A)         std::vector<llvm::Value*>{ _a0 }
#define LUCID_RT_ARG_LIST_2(A, B)      std::vector<llvm::Value*>{ _a0, _a1 }
#define LUCID_RT_ARG_LIST_3(A, B, C)   std::vector<llvm::Value*>{ _a0, _a1, _a2 }

#define LUCID_RT_ARG_LIST(Params) \
    LUCID_RT_GET_MACRO Params (LUCID_RT_ARG_LIST_3, LUCID_RT_ARG_LIST_2, \
                                LUCID_RT_ARG_LIST_1, LUCID_RT_ARG_LIST_0) Params

#define LUCID_RT(EnumName, Symbol, Ret, Params)                              \
    llvm::Value* Abi::EnumName(llvm::IRBuilder<>& builder,                   \
                                LUCID_RT_ARGS(Params)) {                      \
        llvm::Function* fn = declareOrGet(RuntimeFn::EnumName);              \
        std::vector<llvm::Value*> args = LUCID_RT_ARG_LIST(Params);          \
        llvm::CallInst* call = builder.CreateCall(fn, args);                 \
        return Ret##_RETURNS_VALUE(call);                                    \
    }

#include "runtime-abi/functions.def"

#undef LUCID_RT
#undef LUCID_RT_ARG_LIST
#undef LUCID_RT_ARG_LIST_0
#undef LUCID_RT_ARG_LIST_1
#undef LUCID_RT_ARG_LIST_2
#undef LUCID_RT_ARG_LIST_3

#undef Void_RETURNS_VALUE
#undef I1_RETURNS_VALUE
#undef I8_RETURNS_VALUE
#undef I32_RETURNS_VALUE
#undef I64_RETURNS_VALUE
#undef F64_RETURNS_VALUE
#undef Ptr_RETURNS_VALUE
#undef Str_RETURNS_VALUE
#undef Slice_RETURNS_VALUE
#undef Arena_RETURNS_VALUE
#undef Desc_RETURNS_VALUE

} // namespace codegen