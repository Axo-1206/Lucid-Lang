/// @file codegen/ownership/Ownership.hpp
/// @brief The single source of truth for "how does this value get copied
///        and how does it get released?"
///
/// ─── What This File Is ────────────────────────────────────────────────────
/// The ownership engine. Given a type and a value of that type, it decides:
///
///   - Is the value a fresh claim (`Owned`) or an alias (`Borrowed`)?
///   - If the receiver needs a claim of its own, how is it acquired?
///   - When the receiver's claim dies, how is it released?
///
/// Everything that touches ownership in codegen goes through this class.
/// The emitters call `intoOwned` at every write path and `drop` at every
/// cleanup path. There is no other way to copy or release a value.
///
/// ─── What This File Is NOT ────────────────────────────────────────────────
/// It is NOT a translation of the AST. It doesn't know about any specific
/// AST node kind. It takes a type and an LLVM value; nothing else.
///
/// It is NOT the runtime. The `extern "C"` implementations live in
/// `src/runtime/`. `Ownership` emits calls to them via `Abi`; it does not
/// implement them.
///
/// It is NOT the scope tracker. `FunctionState` owns the scope stack; the
/// emitter walks it at scope exit and calls `drop` for each live binding.
/// `Ownership` doesn't know about scopes; it drops values.
///
/// It is NOT the resource classifier. `classifyResourceKind(TypeAST*)` lives
/// in `core/ast/ResourceKind.hpp`, shared by Sema and codegen. `Ownership`
/// includes that header and calls the classifier at its dispatch points.
///
/// ─── The Ownership Model ──────────────────────────────────────────────────
/// Three patterns, dispatched on `ResourceKind`:
///
///   Pattern A — Refcounted (closures). A `cls` value is a fat pointer
///   whose env pointer holds a refcount. Every binding that holds a copy
///   holds a claim. Copy = retain; drop = release. When the refcount hits
///   zero, the env's drop function runs and the env is freed.
///
///   Pattern B — OwnedBuffer (strings, dynamic arrays). The value owns
///   its buffer outright. Copy = deep copy (fresh allocation). Drop = free
///   the buffer, unless `cap == 0` (static string literal). No shared state,
///   no refcount.
///
///   Pattern C — None (primitives, `fn` pointers, references). Copy is a
///   bit copy. Drop is a no-op.
///
/// Plus two special kinds handled here:
///
///   Arena — linear, cannot be copied. Drop frees the base pointer.
///   Handle — linear, cannot be copied. Sema guarantees it was consumed by
///   await/join before scope exit, so drop is a no-op.
///
///   Aggregate — recursive: copy and drop are per-field. Codegen generates
///   the glue lazily (see DropGlue.hpp).
///
/// ─── The `Own` Tag ────────────────────────────────────────────────────────
/// A `Val` produced by an expression emitter carries an `Own` tag:
///
///   Owned    — the value carries a +1 claim. Storing it transfers the
///              claim. The receiver is now responsible for dropping.
///
///   Borrowed — the value aliases a claim held elsewhere (usually a
///              binding's alloca). Storing it requires acquiring a new
///              claim via `intoOwned`.
///
/// The emitter knows the tag because it produced the value; there is no
/// heuristic inspecting the AST to decide. This replaces the old
/// `isFreshExpression`, which tried to derive the tag from the AST shape
/// and got some cases wrong.
///
/// ─── Why a Class, Not Free Functions ──────────────────────────────────────
/// The drop-glue cache and the copy-glue cache live here. They're populated
/// lazily and read on every aggregate drop or copy. State belongs to an
/// object; that object is one per program, held by `ProgramState`.
///
/// ─── Relationship to the Pre-Redesign API ─────────────────────────────────
/// Before the redesign, this functionality was exposed as free functions
/// (`emitRelease`, `emitRetain`, `maybeCoerceFnToCls`) taking a
/// `CodeGenContext&`. Those were transitional: they let old call sites
/// compile while the emitters were being rewritten. They're gone now, and
/// the class is the only entry point.

#pragma once

#include "codegen/Types.hpp"

#include "core/ast/DeclAST.hpp"
#include "core/ast/ResourceKind.hpp"
#include "core/ast/TypeAST.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace codegen {

class ProgramState;
class Abi;

// ─────────────────────────────────────────────────────────────────────────────
// Own — the ownership tag on an emitted value
// ─────────────────────────────────────────────────────────────────────────────
//
// See the file header. `Owned` values are freshly-claimed; storing one
// transfers the claim. `Borrowed` values alias an existing claim; storing
// one requires acquiring a new claim via `Ownership::intoOwned`.

enum class Own {
    Owned,
    Borrowed,
};

// ─────────────────────────────────────────────────────────────────────────────
// Val — a lowered value with its type and ownership tag
// ─────────────────────────────────────────────────────────────────────────────
//
// The emitter's currency. Every `Emitter::emit(ExprAST*)` returns a `Val`.
// Every `Emitter::store(Place, Val)` consumes one.
//
// The default `own` is `Owned`, because the emitter's default for a
// freshly-produced value is "this is a claim you can take." Emitters that
// produce a borrowed value (identifier loads, field loads) set `own` to
// `Borrowed` explicitly.

struct Val {
    llvm::Value* v = nullptr;
    TypeAST* ty = nullptr;
    Own own = Own::Owned;

    Val() = default;
    Val(llvm::Value* value, TypeAST* type, Own ownership)
        : v(value), ty(type), own(ownership) {}

    bool isValid() const { return v != nullptr && ty != nullptr; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Ownership — the ownership engine
// ─────────────────────────────────────────────────────────────────────────────

class Ownership {
public:
    Ownership(ProgramState& program);

    Ownership(const Ownership&) = delete;
    Ownership& operator=(const Ownership&) = delete;

    // ─── Copy / Acquire ───────────────────────────────────────────────────
    //
    // `intoOwned` is the single copy decision point. Given a `Val`, if it's
    // already `Owned`, return it unchanged. If it's `Borrowed`, produce a
    // fresh `Owned` `Val` by copying the value's claim:
    //
    //   Refcounted   — retain the env; return the fat pointer with Owned.
    //   OwnedBuffer  — deep-copy the buffer; return the new buffer with Owned.
    //   Aggregate    — call `__copy_<type>`; return the result with Owned.
    //   None         — bit copy; return with Owned.
    //   Arena        — unreachable (Sema guarantees linearity); if
    //                  reached, returns an invalid Val.
    //   Handle       — unreachable (Sema guarantees linearity); if
    //                  reached, returns an invalid Val.
    //
    // Called by `Emitter::store` before every store, and by every
    // argument-passing site for a `cls`-shaped parameter.
    //
    // When the return value is invalid, the caller must check
    // `isValid()` and bail before using it. The `Arena` and `Handle`
    // cases are the only ones that can return an invalid Val on the
    // success path; every other case returns a valid result.
    Val intoOwned(Val val, llvm::IRBuilder<>& builder);

    // ─── Release ──────────────────────────────────────────────────────────
    //
    // `drop` is the single free decision point. Given a type and a value,
    // release whatever the value owns:
    //
    //   None         — no-op.
    //   Refcounted   — extract env pointer (field 1); null-checked release.
    //   OwnedBuffer  — extract data pointer (field 0); if cap == 0, skip;
    //                  otherwise free.
    //   Arena        — extract base pointer (field 0); free.
    //   Handle       — no-op (linear, consumed by await/join).
    //   Aggregate    — call `__drop_<type>`.
    //
    // Called at scope exit for every live binding, on the old value in an
    // assignment, on a discarded temporary, and at program free.
    void drop(TypeAST* type, llvm::Value* value, llvm::IRBuilder<>& builder);

    // ─── Retain ───────────────────────────────────────────────────────────
    //
    // The retain half of Pattern A, exposed as a public method because
    // `DropGlue.cpp` calls it from generated `__copy_<type>` functions.
    // Code that wants a copy of a value should call `intoOwned`, not this.
    //
    // No-op for every kind except `Refcounted`.
    void retain(TypeAST* type, llvm::Value* value, llvm::IRBuilder<>& builder);

    // ─── Aggregate Glue Access ────────────────────────────────────────────
    //
    // Lazy generation of `__drop_<type>` and `__copy_<type>`. Called by
    // `drop` and `intoOwned` when the type is an aggregate. Public because
    // `DropGlue.cpp` implements them, and because the emitter may want
    // to force generation for a type it's about to use.
    //
    // Both are idempotent: the second call for the same type returns the
    // cached function.
    llvm::Function* dropGlueFor(TypeAST* type);
    llvm::Function* copyGlueFor(TypeAST* type);

private:
    // ─── Internals ────────────────────────────────────────────────────────

    /// Emit a null-checked call to a void-returning runtime function.
    /// The pattern is:
    ///     %is_null = icmp eq ptr %arg, null
    ///     br i1 %is_null, label %skip, label %call
    ///   call:
    ///     call void @fn(ptr %arg)
    ///     br label %skip
    ///   skip:
    /// Leaves the builder at the start of the skip block.
    void emitNullCheckedCall(llvm::Value* arg,
                             llvm::Function* fn,
                             llvm::IRBuilder<>& builder,
                             const llvm::Twine& prefix);

    /// Emit the extract-and-drop sequence for a Refcounted value: pull
    /// the env pointer out of the fat pointer and release it.
    void dropRefcounted(llvm::Value* fatPtr, llvm::IRBuilder<>& builder);

    /// Emit the extract-and-drop sequence for an OwnedBuffer: pull the
    /// data pointer and cap out of the string struct, null-check the
    /// data pointer, and free it if the cap is nonzero.
    void dropOwnedBuffer(llvm::Value* buffer, llvm::IRBuilder<>& builder);

    /// Emit the extract-and-drop sequence for an Arena: pull the base
    /// pointer out and free it.
    void dropArena(llvm::Value* arena, llvm::IRBuilder<>& builder);

    // ─── State ────────────────────────────────────────────────────────────

    ProgramState& program;
};

} // namespace codegen