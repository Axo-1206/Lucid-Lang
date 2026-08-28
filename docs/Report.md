# Lucid CodeGen — Closure & Scope-Exit Memory Management Findings

**Scope:** This document consolidates everything found while auditing closure
lowering and scope-exit memory management against `Grammar.md`'s documented
behavior. It started from `Report.md`'s question about `alloca` vs. "scope
arena" terminology and expanded once closures and general scope-exit cleanup
were checked against the actual CodeGen/Sema/runtime implementation.

**Files reviewed:** `Grammar.md`, `Architecture.md`, `Report.md`,
`CodeGenAlloca.hpp/.cpp`, `CodeGenClosure.hpp/.cpp`, `CodeGenDecl.cpp`,
`CodeGenStmt.cpp`, `CodeGenExpr.cpp`, `CodeGenContext.hpp`, `CodeGenType.hpp/.cpp`,
`CodeGen.hpp/.cpp`, `CodeGenPanic.hpp/.cpp`, `LLVMHelpers.hpp`, `RuntimeError.hpp`,
`RuntimeFunctionRegistry.hpp/.cpp`, `CaptureAnalysis.hpp/.cpp`, `BaseAST.hpp`,
`DeclAST.hpp`, `ExprAST.hpp`, `StmtAST.hpp`, `TypeAST.hpp`,
`IntrinsicEmitter.hpp/.cpp`, `LLVMIntrinsicEmitter.hpp`,
`LucidIntrinsicEmitter.hpp/.cpp`.

**Not reviewed (would sharpen a couple of open items below):** `SemaDecl.cpp`,
`SemaExpr.cpp`, `SemaStmt.cpp`, `LLVMIntrinsicEmitter.cpp`.

---

## Summary Table

| #   | Problem                                                                           | Location                                           | Severity                          | Confirmed?                                  |
| --- | --------------------------------------------------------------------------------- | -------------------------------------------------- | --------------------------------- | ------------------------------------------- |
| 1   | `alloca`/"scope arena" terminology                                                | `Grammar.md` vs. `CodeGenAlloca.*`                 | None — not a bug                  | Resolved, no change needed                  |
| 2   | Closure environment is never retained or released                                 | `CodeGenClosure.cpp`, `RuntimeFunctionRegistry.*`  | High (leak)                       | Confirmed                                   |
| 3   | Closures may capture function/closure-typed values with no validation             | `CaptureAnalysis.cpp`                              | Medium (spec gap)                 | Confirmed                                   |
| 4   | Every capture is unconditionally by-reference                                     | `CaptureAnalysis.cpp`                              | High (behavioral gap vs. spec)    | Confirmed                                   |
| 5   | `FuncTypeAST` never maps to the closure's actual `{ptr,ptr}` runtime shape        | `CodeGenType.cpp`                                  | Critical (miscompile / OOB write) | Confirmed                                   |
| 6   | No automatic free of heap-backed locals at scope exit (arrays, strings, closures) | Whole CodeGen pipeline                             | Critical (universal leak)         | Confirmed                                   |
| 7   | `#scope_exit` callbacks don't fire on early return out of a nested branch         | `CodeGenStmt.cpp` / `CaptureAnalysis` control flow | High                              | Confirmed by code trace; not runtime-tested |
| 8   | Closures may capture borrowed types (`&T`, `[_]T`)                                | `CaptureAnalysis.cpp` Rule 3                       | —                                 | **Not a bug** — correctly forbidden already |

---

## 1. `alloca` vs. "scope arena" terminology (resolved, no action needed)

`Grammar.md` describes local variables as living in a "scope arena... a stack
of bump-pointer arenas, one per block," implemented via `alloca`. The
implementation (`createAlloca` in `CodeGenAlloca.cpp`) places every local in
the function's **entry block**, not the lexical block.

This is not a discrepancy. `Grammar.md` itself says *"LLVM's `alloca`
instruction is already scope-lifetime by definition — the frontend's arena
framing maps directly onto it."* `CodeGenAlloca.hpp` documents the same
three-tier model (stack/`alloca`, heap `#alloc`/`#free`, named arena) that the
grammar describes. The "arena" language is a conceptual description of a
lifetime discipline, not a claim about a literal bump-pointer allocator — no
grammar or code change is needed here.

---

## 2. Closure environments are allocated but never retained or released

**Claim in `Grammar.md`** (~line 4361-4398): closure environments are
heap-allocated and **reference-counted** — copying a closure retains the
environment, and destroying a closure-typed binding releases it, freeing the
environment automatically when the count reaches zero.

**What the code does:**

- `CodeGenClosure.cpp::lowerClosure` calls `RuntimeFn::AllocEnv` to allocate
  the environment. It never calls anything resembling retain/release.
- `RuntimeFunctionRegistry.hpp`'s `RuntimeFn` enum — the single source of
  truth for every runtime function CodeGen can call — has **no `RetainEnv` or
  `ReleaseEnv` entry at all**:

  ```cpp
  enum class RuntimeFn {
      // ─── Closures ───────────────────────────────────────────────────
      AllocEnv,           // void* __lucid_alloc_env(uint64_t size)
      // ─── Memory Management ──────────────────────────────────────────
      Alloc, Free, ArenaCreate, ArenaAlloc, ArenaReset, ArenaFree,
      // ... strings, formatters, Panic
  };
  ```

  This isn't "the call exists somewhere else" — the runtime surface itself
  has no refcount operation to call. `getRuntimeFunctionInfo()` hard-asserts
  on any enumerator missing a table row, so there is no way to reach a
  retain/release call even by accident.
- `CodeGenDecl.cpp::lowerVarDecl` (which handles every local/global variable,
  including closure-typed ones) has no special-casing for closures — no
  retain call when a closure value is stored into a variable.
- `CodeGenStmt.cpp` has no destruction/release logic anywhere (see §6).

**Impact:** every closure environment leaks. There is no code path, anywhere
in the reviewed files, that frees a closure environment.

---

## 3. Closures may capture function/closure-typed values with no rejection

`CaptureAnalysis.cpp::validateAndAddCapture` has exactly two rejection rules:

```cpp
// Rule 3: Borrowed types (&T, [_]T) cannot be captured
if (varType && isBorrowedType(varType)) { ...; return; }

// Rule 4: Linear types (Future<T>, Thread<T>) cannot be captured
if (varType && (varType->isa<FutureTypeAST>() || varType->isa<ThreadTypeAST>())) { ...; return; }
```

`FuncTypeAST` (`TypeAST.hpp`) is its own AST node, unrelated to
`RefTypeAST`/slice types, so Rule 3 doesn't catch it; Rule 4 only checks
`Future`/`Thread`. **There is no rule against capturing a function or closure
value.** This interacts with Finding 5 below to produce a concrete
memory-safety bug rather than just a missing validation.

---

## 4. Every capture is unconditionally by-reference

```cpp
bool shouldCaptureByReference(ValueDeclAST* decl, IdentifierExprAST* id) const {
    if (!decl) return false;
    // Conservative: capture all variables by reference
    // TODO: Optimize to capture by value when possible (read-only, small types)
    return true;
}
```

This contradicts the documented intent on `CapturedVariable` itself
(`BaseAST.hpp`):

> `byReference`: True if this closure may **write** to the captured
> variable... False if this closure only **reads** it, in which case it may
> instead be **snapshot-copied**.

And it contradicts `Grammar.md`'s "Owned value → full copy" semantics for
plain locals. Every capture — read-only or not — currently shares a pointer
to the original variable's storage rather than copying it.

**Downstream risk:** `CodeGenClosure.cpp`'s by-reference branch stores
whatever pointer `ctx.lookupValue(capture.decl)` returns directly into the
environment, on the assumption noted in its own comment: *"Sema should have
promoted such variables to heap allocation [if the closure may escape]."*
Nothing in the reviewed `CaptureAnalysis.cpp` performs that promotion —
`markClosureIfEscaping` only sets an `isReturned` flag on the closure/function
declaration, never touches the captured variable's storage class. Combined
with `CodeGenDecl.cpp::lowerVarDecl` always using a plain stack `alloca`, an
escaping closure that captures a local likely holds a dangling pointer once
the enclosing function returns. **This specific consequence needs
`SemaDecl.cpp`/`SemaExpr.cpp` to fully confirm or rule out** — those files
were not part of this review.

---

## 5. The type system has no representation for a closure's actual runtime shape

`CodeGenClosure.cpp::lowerClosure` builds every closure value (capturing or
not) as an anonymous `{ ptr, ptr }` struct — `{ funcPtr, envPtr }`. But
`CodeGenType.cpp`'s type mapper never produces that shape:

```cpp
case ASTKind::FuncType:
    result = getFunctionType(ctx, type->as<FuncTypeAST>(), false);
    break;
```

`getFunctionType` returns a raw `llvm::FunctionType*` — a function
*signature*, not a first-class value type. No `getClosureType()` helper exists
anywhere in the codebase. Concretely, this breaks:

1. **Any plain variable of function type** (`let f (int) -> int = someFunc;`).
   `lowerVarDecl` calls `createAlloca(varName, varType, ctx)` with a
   non-sized, non-first-class `FunctionType*` — `AllocaInst` requires a sized
   type. This should fail an LLVM assertion or IR verification.
2. **Function-typed struct fields and parameters** — same root cause, via
   `getStructType`'s field loop and `getFunctionType`'s own parameter loop.
3. **Captured function/closure values — the most severe case.**
   `getCaptureFieldType` computes the environment field type as
   `PointerType::get(getType(ctx, capture.decl->type), 0)`. Because captures
   are always by-reference (Finding 4), this always fires, and
   `PointerType::get(FunctionType*, 0)` is valid LLVM (pointer-to-function),
   so it doesn't crash while *building* the struct. But it reserves only
   **one pointer's width** for that field. If the captured value is itself a
   capturing closure, its actual runtime value is the **two-word `{func,
   env}` struct**. `CreateStore(capturedValue, fieldPtr)` in `lowerClosure`
   writes the value's real width at that offset — a 16-byte write into an
   8-byte-wide slot corrupts the adjacent environment field, or overflows the
   heap allocation if it's the last field.
4. **Curried return types are partially patched, but still wrong for
   captures.** The curried branch of `getFunctionType` wraps the inner type
   as a bare function pointer, not the `{ptr,ptr}` closure struct — an ABI
   mismatch if the returned function is actually a capturing closure.

**Root-cause framing:** this is not a set of independent bugs so much as one
structural gap — closures were added entirely inside `CodeGenClosure.cpp`
without ever teaching `CodeGenType.cpp` that a Lucid function type can denote
a two-word closure value, not just a bare function pointer.

---

## 6. No automatic freeing of heap-backed locals at scope exit

`Grammar.md`'s semantics table (~line 4121) states dynamic arrays are
*"scope arena, automatic — heap-backed, but the free call is
compiler-inserted at scope exit like everything else."* The same is implied
for `string` and (via reference counting) closures.

**No such compiler-inserted free exists.** The only call site for
`RuntimeFn::Free` in the entire codebase is the explicit, user-written
`#free(ptr)` intrinsic, in `LucidIntrinsicEmitter.cpp`:

```cpp
if (kind == IntrinsicKind::Free) {
    ...
    llvm::Function* freeFunc = ctx.getRuntimeFn(RuntimeFn::Free);
    ...
    ctx.builder.CreateCall(freeFunc, {ptr});
    return nullptr;
}
```

`CodeGenStmt.cpp`, `CodeGenDecl.cpp`, and `CodeGenAlloca.cpp` have no code
that walks a block's locals at scope exit and frees the heap-backed ones.
`createAlloca`'s "auto-freed" guarantee (see §1 and `CodeGenAlloca.hpp`) is
real but narrow: it reclaims the fixed-size stack slot on function return,
not any heap memory that slot happens to point to. `#alloc`/`#free` are
correctly manual per the grammar's own "manual heap" section — but nothing
implements the *separate*, "automatic" scope-arena promise the grammar makes
for `[*]T`, `string`, and closures.

**Impact:** every dynamic array, string, and closure declared as a local
leaks its backing heap allocation on every scope exit (block, function
return, or otherwise), not just closures specifically.

---

## 7. `#scope_exit` — the one opt-in cleanup mechanism — doesn't fire on every exit edge

`#scope_exit` is Lucid's only per-scope cleanup primitive today (§6), and per
`Grammar.md` it should be *"drained in LIFO order and popped on every exit
edge of that block (fall-through, return, break, continue)."*

Tracing the actual control flow for a `return` nested inside an `if` inside a
block:

1. `lowerBlockStmt`'s loop reaches the `if` statement and calls `lowerIfStmt`.
2. Inside the `then` branch, `lowerReturnStmt` calls `ctx.builder.CreateRet(...)`,
   terminating that block.
3. `lowerIfStmt`'s post-branch check (`if (!GetInsertBlock()->getTerminator())
   CreateBr(mergeBlock)`) correctly sees the block is already terminated and
   skips branching to `mergeBlock` — so the returning path never reaches
   `mergeBlock`.
4. `lowerIfStmt` unconditionally sets the insertion point to `mergeBlock`
   before returning control to the caller.
5. The **outer** `lowerBlockStmt`'s scope-exit-draining loop emits its
   callbacks into `mergeBlock`.

`mergeBlock` is only reachable via the `if`-condition-**false** path. The
early-return path never flows through it, so the enclosing block's
`#scope_exit` registrations silently never run when the function returns from
inside that branch.

`emitScopeExitCallback` (`LucidIntrinsicEmitter.cpp`) has no reachability or
terminator guard of its own — it unconditionally emits a `call` at the
current insertion point. This confirms the bug is in *where* `lowerBlockStmt`
invokes the drain, not a defensive check inside the callback emitter itself.

**Caveat:** this is a control-flow trace from the source, not a compiled
repro — worth a quick unit test to confirm before fixing.

---

## 8. Closures capturing borrowed types (`&T`, `[_]T`) — correctly forbidden, no action needed

`Grammar.md` (~line 4401-4419) explicitly bans this, with rationale: a
closure's environment is designed to outlive the scope that created it, which
is exactly the guarantee `&T`/`[_]T` are forbidden from making. `*T` (raw
pointer) is deliberately exempt, since it carries no such guarantee to begin
with.

`CaptureAnalysis.cpp` Rule 3 implements this correctly today. **No change
needed.** Included here only for completeness, since it came up during the
investigation.

---

## Root-Cause Framing

Findings 2, 6, and 7 are three symptoms of the same missing piece: Lucid's
compiler has no general "run cleanup for everything live in this scope, on
every exit edge" mechanism. The grammar describes one; a couple of narrow,
disconnected pieces of scaffolding exist (`AllocEnv`, `#scope_exit`), but
nothing wires them into what the documentation promises. Finding 5 is a
separate, more fundamental gap — the type system was never extended to
represent a closure's actual two-word runtime shape — and it's what turns
Finding 3 (missing validation) into an actual memory-corruption risk rather
than a cosmetic hole.

## Suggested Priority Order

1. **Finding 5** (type-system gap) — blocks correct codegen for the most
   basic case (a variable of function type) and is the precondition for
   fixing captures safely.
2. **Finding 7** (scope-exit skips early return) — needed before any
   automatic-free mechanism can be trusted on the return path.
3. **Finding 6 / 2** (no automatic free / no closure refcount) — the actual
   leak-fixing work, naturally sequenced after 5 and 7 land.
4. **Finding 4** (always-by-reference capture) and **Finding 3** (missing
   function-type capture validation) — can be fixed independently, but
   Finding 4's dangling-pointer risk should be confirmed against
   `SemaDecl.cpp`/`SemaExpr.cpp` first.

## Open Items Needing Files Not Yet Reviewed

- `SemaDecl.cpp` / `SemaExpr.cpp`: confirm whether escaping closures'
  captured *variables* (not just their environments) get promoted to heap
  storage anywhere (Finding 4).
- `LLVMIntrinsicEmitter.cpp`: not required for the closure/scope-exit
  findings above, but not yet reviewed in this pass.
- A compiled repro for Finding 7 (early-return skipping `#scope_exit`) to
  confirm the traced behavior against actual generated IR.

--- 

# Extra sections

task: "Ok can you explain why you use the term Closure but not function type for the capture?"

---

Good — this confirms and generalizes what Rule 3/4 in `CaptureAnalysis.cpp` implement. Let me pull the linear-type rule too, since that's the other explicit ban.Now let me check the `#scope_exit` snapshot note and the smuggling reference at line ~6125, since both touch capture rules too.I searched every mention of "capture" in `Grammar.md` (23 hits) and traced them to the authoritative source: the **Ownership Categories at a Glance** table (~line 4114-4130) plus two explicit rule lists. Here's the full picture.

## Allowed to capture

| Type                                                    | Category           | Why it's safe to capture                                                                                               |
| ------------------------------------------------------- | ------------------ | ---------------------------------------------------------------------------------------------------------------------- |
| `bool`, `int`, `float`, `char`, enums, `[N]T`, `struct` | Owned value        | Full copy — the copy is independent of the source, so nothing can dangle                                               |
| `[*]T` (dynamic array), `string`                        | Owned value        | Full **deep** copy — heap-backed, but the copy owns its own buffer                                                     |
| Named function                                          | Owned value        | Pointer copy, no state, never dangles                                                                                  |
| **Closure**                                             | Shared, refcounted | Copies the `{func, env}` fat pointer and retains the env — this is explicitly *"the point of it"* per the table        |
| `*T` (raw pointer)                                      | Sealed conduit     | Carries no "must not outlive source" guarantee to begin with — capturing one is no less safe than a struct storing one |
| `Shared<T>` / `Weak<T>` (stdlib, planned)               | Shared, refcounted | Same retain/release mechanism as a closure's env                                                                       |

The grammar states the general principle plainly at line 4111: *"If a value needs to escape upward out of the scope that created it — returned, stored in a struct, captured by a closure — it cannot be a borrowed view; it must be owned outright or shared."*

## Not allowed to capture

| Type             | Why forbidden                                                                                                                                                                                             | Grammar reference         |
| ---------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------- |
| `&T` (reference) | **Downward Flow Rule**, ban #4: borrowed types can flow into nested calls only, never upward/sideways. A closure's env is designed to outlive its creating scope — exactly the guarantee `&T` can't make. | Line 4278-4280, 4401-4419 |
| `[_]T` (slice)   | Same rule, explicitly stated to apply "identically" to slices — a slice is a borrowed view over a buffer it doesn't own.                                                                                  | Line 4039-4044            |
| `Future<T>`      | **Different rationale** — not a dangling-pointer hazard, a *double-consumption* hazard: a closure might run more than once, and `Future<T>`/`Thread<T>` may only ever be awaited/joined once.             | Line 5971-5977            |
| `Thread<T>`      | Follows every `Future<T>` rule verbatim (substituting `spawn`/`join`) — "never captured by a closure" stated explicitly.                                                                                  | Line 6357-6362            |

Worth flagging: the grammar treats `&T`/`[_]T` and `Future<T>`/`Thread<T>` as **two structurally distinct bans with different root causes** — dangling-pointer risk vs. run-more-than-once risk — even though both land on "closures can't capture this." That distinction matters if you're ever writing the diagnostic messages, since "cannot capture a borrowed type" and "cannot capture a linear type" are different errors for different reasons, not one rule with two names.

## How this maps back to `CaptureAnalysis.cpp`

Rule 3 (borrowed) and Rule 4 (linear) implement exactly these two bans correctly — confirmed already in this conversation, no change needed there.

What's still missing is a **third rule the grammar's own table implies but never states as an explicit ban**: nothing in the table forbids capturing a function/closure value, and per the table it's clearly *allowed* (Shared, refcounted row). So the real gap isn't "the grammar wants this banned and the code doesn't ban it" — it's that **the grammar correctly allows it, but the type-mapping bug (Finding 5 in the doc) makes that allowed case unsafe to actually execute.** Capturing a closure is supposed to work exactly like the `Shared, refcounted` row describes; right now it doesn't, because `getCaptureFieldType` sizes the environment slot for a bare function pointer instead of the two-word `{func, env}` struct the grammar's own table says a closure actually is.

Want me to add this allowed/forbidden table as a section in the findings doc, or fold the `&T` vs. `Future<T>` distinction into the write-up for Finding 3?

---

question: "Ok can you explain why you use the term Closure but not function type for the capture?"

Good catch to press on that — it's not just word choice, it maps onto a real split in the grammar's own model.

## The grammar splits "function type" into two different runtime categories

Look again at the Ownership Categories table (line 4114-4130):

| Type               | Category           | Copy behavior                                     | Freed                                 |
| ------------------ | ------------------ | ------------------------------------------------- | ------------------------------------- |
| **Named function** | Owned value        | pointer copy, no state                            | never — static                        |
| **Closure**        | Shared, refcounted | copies fat pointer `{func, env}`; env is retained | heap, automatic — freed at refcount 0 |

These are two separate rows for what is, at the *Lucid syntax level*, the same static type — `FuncTypeAST`, e.g. `(int) -> int`. Nothing in `TypeAST.hpp` distinguishes "a function type that's just a plain pointer" from "a function type that might carry captured state." A variable declared `let f (int) -> int = ...` could hold either:

- A reference to a top-level function (`add`, `math:normalize`) — no environment, just a bare pointer. This is the **Owned value** row: trivially safe to capture, copy, store anywhere, because there's no heap state behind it to dangle or need retaining.
- An anonymous closure literal or nested function with captures — a `{func, env}` fat pointer with a real heap allocation behind it. This is the **Closure** row: Shared/refcounted, and this is the one that actually needs retain/release semantics to be safe.

So "closure" isn't a stylistic synonym for "function type" — it's the grammar's name for the specific *runtime* category that owns heap state, as opposed to the *type* that both categories share on the surface.

## Why that distinction is exactly the one I should have called out more explicitly

When I said "closures may capture function/closure-typed values with no rejection" earlier, I was blurring the two together. More precisely, per the table:

- Capturing a variable whose value happens to be a **named function** is completely fine — no bug, no leak, matches the Owned-value row exactly.
- Capturing a variable whose value happens to be a **closure** (has its own environment) is the case that actually exercises Finding 5 — the environment slot gets sized for a bare function pointer (1 word) when the real value is a 2-word `{func, env}` struct.

The problem is that **Lucid's static type system can't tell these apart at the point Sema validates a capture.** `CaptureAnalysis.cpp::validateAndAddCapture` only has `decl->type` to inspect, and that's a `FuncTypeAST` in both cases — identical AST node, identical `TypeAST*`. Sema would need to look at the *value* being captured (is this identifier bound to a plain function declaration, or to a closure literal / another closure-typed variable?) to know which of the two ownership rows actually applies — the same kind of value-level dispatch `emitCallableCall` already does in CodeGen (`callee->getType()->isStructTy()` vs. `dyn_cast<llvm::Function>`), just done at Sema time instead of CodeGen time.

So the honest way to state the gap now is: **it's not that "function types" are unsafe to capture — named functions are fine. It's that Lucid has no type-level way to distinguish a capture-safe named function from a capture-unsafe closure, so any function-typed capture has to be treated as a potential closure capture until proven otherwise, and right now nothing does that proving.**

Want me to fold this sharper distinction into the findings doc — probably as a refinement to Finding 3, splitting it into "capturing a named function (safe)" vs. "capturing a closure (needs the Finding 5 fix first)"?