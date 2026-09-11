/// @file support/CodeGenOwnership.hpp
/// @brief Single source of truth for "does this binding own a heap resource,
///        and how do we release / retain it?"
///
/// ─── Purpose ──────────────────────────────────────────────────────────────
/// Lucid has several kinds of heap-owned values that must be released when
/// their binding dies or is overwritten:
///
///   - Closure environments  (fat pointer { func, env }, env is refcounted)
///   - Strings               (data pointer, owned by the binding)
///   - Dynamic arrays [*]T   (data pointer, owned by the binding)
///   - Structs with any of the above as a field   (Phase 5)
///   - TaggedSlot-wrapped resources (Phase 4)
///
/// Before this file existed, every call site that needed to release one of
/// these inlined its own type dispatch — emitCleanupForTracker had one,
/// CodeGenContext::reassign had another, lowerAssignExpr had a third. Adding
/// a new resource type meant updating every one of them, and they drifted.
///
/// This file is the single place that answers three questions:
///
///   1. ownsResource(decl)
///      Does this declaration's value need to be released?
///
///   2. emitRelease(decl, value, ctx)
///      Emit the release IR for this binding's current value.
///
///   3. emitRetain(decl, value, ctx)
///      Emit the retain IR for a value being copied into a new binding.
///
/// Every call site that transfers or destroys ownership calls through this
/// API instead of re-deriving the rules. See emitRelease / emitRetain for
/// the exact IR patterns, and ownsResource for the dispatch table.
///
/// ─── Relationship to CodeGenClosure.cpp ──────────────────────────────────
/// lowerClosure (in CodeGenClosure.cpp) *allocates* a closure environment
/// and constructs the fat pointer. This file *releases* and *retains* one.
/// They are deliberately separate: allocation happens once, at the point
/// the closure literal is evaluated; release/retain happen many times, at
/// every binding death and every copy. Keeping them in separate files makes
/// the asymmetry (one alloc, many releases) explicit.
///
/// ══════════════════════════════════════════════════════════════════════════
/// ─── THE OWNERSHIP MODEL ─────────────────────────────────────────────────
/// ══════════════════════════════════════════════════════════════════════════
///
/// Every heap-owned value in Lucid follows one of three ownership patterns
/// when it flows between bindings. The pattern is determined by the
/// *resource type* and by whether the source is a fresh temporary or an
/// existing binding.
///
/// ─── Pattern A: Refcounted (closures only) ───────────────────────────────
/// A closure environment is refcounted. Every binding that holds a copy of
/// a closure fat pointer holds a claim on the env. The claim is acquired
/// either by allocation (refcount starts at 1) or by retain (refcount++).
/// It is released by emitRelease (refcount--). When the refcount hits 0,
/// the env is freed.
///
/// ─── Pattern B: Deep copy (strings, dynamic arrays) ──────────────────────
/// A string or dynamic array owns its buffer outright. When the value is
/// copied into another binding, the copy is a fresh allocation — the new
/// binding owns its own buffer, independent of the source. No retain, no
/// shared state. The old binding is unaffected; the new binding releases
/// its own buffer when it dies.
///
/// ─── Pattern C: No-op (primitives, non-capturing functions, references) ──
/// The value owns nothing. emitRelease and emitRetain are no-ops.
///
/// ══════════════════════════════════════════════════════════════════════════
/// ─── TRANSFER vs COPY — THE HYBRID MODEL ─────────────────────────────────
/// ══════════════════════════════════════════════════════════════════════════
///
/// For refcounted resources (closures), the question "does the destination
/// binding retain, or does it take over the source's claim?" depends on
/// where the value came from. Lucid uses a *hybrid* model:
///
/// ─── Rule 1: Transfer from a fresh temporary ─────────────────────────────
/// When a closure value is the direct result of `lowerClosure` — that is,
/// when the AST node producing it is an AnonFuncExprAST or a FuncDeclAST
/// whose initializer is a closure literal — the value arrives with one
/// implicit claim (allocated refcount = 1). Storing it into a binding
/// *transfers* that claim. No retain is emitted at the store site.
///
///   Caller:  %val = lowerClosure(expr, ctx)   ; refcount = 1 (temp claim)
///   Store:   store %val, %binding             ; temp claim → binding
///   Cleanup: emitRelease at scope exit        ; refcount-- → 0, freed
///
/// ─── Rule 2: Copy from an existing binding ───────────────────────────────
/// When a closure value comes from an existing binding (a `*T` load, a
/// struct field read, an identifier reference to a `FuncDeclAST` binding),
/// that source binding holds its own claim on the env. Storing the value
/// into a destination binding acquires a *new* claim via emitRetain. Both
/// bindings hold independent claims.
///
///   Source:  %val = load %source_alloca          ; source's claim = 1
///   Retain:  emitRetain(decl, %val, ctx)         ; refcount++ → 2
///   Store:   store %val, %dest_alloca            ; dest's claim acquired
///   Cleanup: emitRelease at scope exit (both)    ; refcount-- twice → 0
///
/// ─── Rule 3: Always retain on return and argument pass ───────────────────
/// Return and argument pass are always "copy" operations, because the
/// destination frame outlives the source frame's cleanup. The callee or
/// caller receives an independent claim via emitRetain, and the source
/// frame releases its own claim through normal scope-exit cleanup.
///
///   Caller:  %val = lowerExpression(arg)         ; source claim = 1
///   Retain:  emitRetain(_, %val, ctx)            ; refcount++ → 2
///   Call:    f(%val)                             ; callee's claim
///   In f:    param stored in alloca              ; param binding owns claim
///   ...
///   Cleanup: callee's scope exit releases        ; refcount-- → 1
///   Cleanup: caller's scope exit releases        ; refcount-- → 0
///
/// ─── Rule 4: Self-assignment is a no-op ──────────────────────────────────
/// When reassigning a binding to its own value (`f = f;`, or `f = g;` where
/// `g` shares `f`'s env because both were copied from the same source), the
/// destination and source env pointers are equal. Under Rule 2 this would
/// be: release old (refcount-- possibly to 0, freeing the env), then retain
/// new (reading a freed pointer). To avoid this, `lowerAssignExpr` must
/// compare the old and new env pointers at IR level and skip both release
/// and retain when they are equal. This is a call-site concern, not an
/// API concern — see Phase 3's lowerAssignExpr.
///
/// ─── How the Call Sites Encode This ──────────────────────────────────────
///
///   emitCleanupForTracker:  always emitRelease (Rule 1 or 2 — the binding
///                           is dying, its claim must go away)
///
///   lowerReturnStmt:        always emitRetain (Rule 3)
///
///   lowerCallExpr:          always emitRetain per closure arg (Rule 3)
///
///   lowerAssignExpr:        emitRelease old value,
///                           then:
///                             if RHS is a fresh closure literal → no retain
///                             else → emitRetain (Rule 2)
///                           with a self-assignment guard (Rule 4)
///
///   lowerStructLiteralExpr: Phase 5 decides per-field (fresh literal vs
///                           existing binding, same rules)
///
/// ─── Why Not Just Always Retain? ─────────────────────────────────────────
/// If every store retained, the temp claim from lowerClosure would leak:
///
///   %val = lowerClosure(...)          ; refcount = 1
///   emitRetain(_, %val, ctx)          ; refcount = 2
///   store %val, %binding              ; binding claim acquired
///   ... scope exit ...                ; refcount-- → 1, LEAK
///
/// The temp claim has no owner to release it. The hybrid model avoids this
/// by treating the fresh-literal case as a transfer.
///
/// ─── Why Not Just Always Transfer? ───────────────────────────────────────
/// If every store transferred, a copy from an existing binding would steal
/// the source's claim:
///
///   %val = load %source_alloca        ; source claim = 1
///   store %val, %dest_alloca          ; dest "takes over" source's claim
///   ... source scope exits ...        ; refcount-- → 0, FREED
///   ... but source still holds it ...
///   ... LEAK/DOUBLE-FREE ...
///
/// The hybrid model avoids this by retaining on copies.
///
/// The hybrid model is the only one that handles both cases correctly
/// without requiring the caller to prove "no other binding holds this" at
/// every store site — which CodeGen cannot do, because it doesn't track
/// every binding's ownership statically.
///
/// ─── IR Shape Invariants ─────────────────────────────────────────────────
/// emitRelease and emitRetain always produce balanced branches:
///   - If the resource pointer is null, they skip the runtime call.
///   - Otherwise, they call the runtime function once.
///
/// The runtime functions themselves (__lucid_release_env, __lucid_free) are
/// null-safe, so this null-check is technically redundant. It is kept for
/// two reasons:
///   1. It makes the generated IR self-evidently correct on inspection.
///   2. It saves a call on the common case of an unset/empty binding.
/// Phase 8 may remove it if IR size becomes a concern.

#pragma once

#include "../context/CodeGenContext.hpp"
#include "core/ast/DeclAST.hpp"

#include <llvm/IR/Value.h>

namespace codegen {

// ─── ownsResource ─────────────────────────────────────────────────────────

/// @brief Does this declaration's value own a heap resource that must be
///        released when the binding dies or is overwritten?
///
/// See the file header's OWNERSHIP MODEL section for the three patterns.
/// This function classifies a declaration into one of them.
///
/// Dispatches on decl->type. Current coverage:
///
///   - FuncTypeAST on a FuncDeclAST with hasClosure
///       → Pattern A (refcounted closure env).
///   - FuncTypeAST on a FuncDeclAST without hasClosure
///       → Pattern C (no-op).
///   - FuncTypeAST on a VarDeclAST
///       → Pattern C. VarDeclAST never holds a function-typed binding in
///         Lucid; the parser's looksLikeFuncDecl dispatch guarantees it.
///   - PrimitiveTypeAST with PrimitiveKind::String
///       → Pattern B (deep copy).
///   - ArrayTypeAST with ArrayKind::Dynamic
///       → Pattern B (deep copy).
///   - NamedTypeAST pointing at a StructDeclAST
///       → Phase 5. Currently Pattern C (stub). Will become a recursive
///         classification once structOwnsResources exists.
///   - Nullable/Fallible/Combined wrapping a resource
///       → Phase 4. Currently Pattern C (stub). Will unwrap and recurse.
///   - Everything else
///       → Pattern C.
///
/// @param decl The declaration whose binding we're asking about. May be a
///             VarDeclAST, FuncDeclAST, ParamAST, or FieldDeclAST — any
///             ValueDeclAST that can be marked alive in the tracker.
/// @return true if the binding owns a heap resource.
bool ownsResource(ValueDeclAST* decl);

// ─── emitRelease ──────────────────────────────────────────────────────────

/// @brief Emit the release IR for a binding's current value.
///
/// The caller must pass the *value*, not an alloca holding it. For a
/// binding stored in an alloca, load the value first:
///
///     llvm::Value* oldValue = builder.CreateLoad(valueType, alloca);
///     emitRelease(decl, oldValue, ctx);
///
/// The emitted IR is shaped per resource kind:
///
///   Closure (fat pointer struct { func, env }):
///     %env      = extractvalue { ptr, ptr } %value, 1
///     %is_null  = icmp eq ptr %env, null
///     br %is_null, %skip, %release
///   release:
///     call void @__lucid_release_env(ptr %env)
///     br %skip
///   skip:
///     ; continue here
///
///   String ({ ptr, i64, i64 } struct):
///     %data     = extractvalue { ptr, i64, i64 } %value, 0
///     ; skip if %data is a global string literal (known at IR level)
///     %is_null  = icmp eq ptr %data, null
///     br %is_null, %skip, %release
///   release:
///     call void @__lucid_free(ptr %data)
///     br %skip
///   skip:
///
///   Dynamic array ({ ptr, i64, i64 } struct):
///     Same shape as string, using field 0 (data pointer).
///
/// This function implements the "release" half of the ownership model:
/// under both Rule 1 (transfer) and Rule 2 (copy), a binding's death
/// releases exactly one claim. The caller does not need to know whether
/// the binding's claim came from a transfer or a retain — emitRelease
/// always removes exactly one claim.
///
/// @param decl  The declaration the value belongs to. Used for type dispatch
///              (decl->type) and, when applicable, for diagnostics.
/// @param value The LLVM value to release. Must be the current value, not an
///              alloca. If null, this is a no-op.
/// @param ctx   The code generation context.
///
/// @note This function is non-destructive with respect to decl and value —
///       it emits IR but does not mutate the binding or the tracker. The
///       caller is responsible for removing the binding from the tracker
///       if the release is a true death (scope exit) rather than a transfer
///       (reassignment, where the binding remains alive).
void emitRelease(ValueDeclAST* decl, llvm::Value* value, CodeGenContext& ctx);

// ─── emitRetain ───────────────────────────────────────────────────────────

/// @brief Emit the retain IR for a value being copied into a new binding.
///
/// Under the hybrid ownership model (see file header), emitRetain is called
/// only when the value being stored is a copy from an existing binding
/// (Rule 2) or a transfer into a caller/callee frame (Rule 3). It is NOT
/// called when the value is a fresh closure literal being stored for the
/// first time (Rule 1 — that's a transfer, no retain).
///
/// The emitted IR for a closure:
///
///     %env      = extractvalue { ptr, ptr } %value, 1
///     %is_null  = icmp eq ptr %env, null
///     br %is_null, %skip, %retain
///   retain:
///     call void @__lucid_retain_env(ptr %env)
///     br %skip
///   skip:
///
/// For strings and dynamic arrays (Pattern B — deep copy), emitRetain is a
/// no-op: the assignment site has already produced a fresh allocation, and
/// the new binding owns its own buffer. There is no shared state to retain.
///
/// For Pattern C (primitives, non-capturing functions, references), emitRetain
/// is a no-op.
///
/// Coverage today:
///
///   - FuncTypeAST on a FuncDeclAST with hasClosure → retain env (Rule 2/3).
///   - FuncTypeAST on a FuncDeclAST without hasClosure → no-op.
///   - String, dynamic array → no-op (documented above).
///   - Struct, TaggedSlot → Phase 4 / Phase 5 stubs.
///
/// @param decl  The declaration the value belongs to. Used for type dispatch.
/// @param value The LLVM value to retain. Must be the value, not an alloca.
/// @param ctx   The code generation context.
void emitRetain(ValueDeclAST* decl, llvm::Value* value, CodeGenContext& ctx);

} // namespace codegen