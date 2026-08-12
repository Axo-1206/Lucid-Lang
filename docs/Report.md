Now let me check the CodeGenExpr.cpp entry point for intrinsic calls, plus ExprAST.hpp for `IntrinsicCallExprAST` definition.Scanned both files plus how `CodeGenExpr.cpp` feeds into them. There's one systemic issue underneath everything else, then several intrinsic-specific ones.

## The big one: `loadIfNeeded`'s bool-overload is a permanent no-op

```cpp
// CodeGenHelpers.cpp
llvm::Value* loadIfNeeded(llvm::Value* value, bool isLValue, CodeGenContext& ctx) {
    if (!value || !isLValue) return value;
    // With opaque pointers, we can't get the element type from the pointer.
    // Return the pointer and let the caller handle it.
    return value;
}
```

This literally always returns `value` unchanged — the comment admits it's unfinished ("let the caller handle it"), but **no caller handles it**. I grepped every call site across `CodeGenExpr.cpp` and `IntrinsicEmitter.cpp`: all 14 of them use this bool-overload, including `IntrinsicEmitter.cpp:121`. There's a second overload right next to it that's correctly implemented (`loadIfNeeded(Value*, llvm::Type* elemType, ctx)` → real `CreateLoad`), and it's **never called anywhere**. Dead code sitting right next to the bug that should be calling it.

Right now this is masked because `lowerIdentifierExpr` is also a stub (`return llvm::Constant::getNullValue(...)`, with its own "not fully implemented" warning) — so nothing lvalue-shaped is flowing through yet. But it's a landmine: the moment identifier lookup gets wired to `ctx.lookupValue(decl)` and starts returning real `alloca` pointers, every one of those 14 call sites — including the intrinsic-argument loop — will start passing raw addresses into places expecting values. `#sqrt(x)` on a local variable would try to build `llvm.sqrt` over a pointer type; `x + y` would try `CreateAdd` on two pointers. Worth fixing now, before it's disguised by a dozen more call sites depending on it, either by routing `arg->resolvedType` into the real `Type*`-overload or deleting the bool stub outright so it can't be silently relied on again.

## In `IntrinsicEmitter.cpp` specifically

**`#addrof` will break as soon as (1) is fixed.** The argument-lowering loop in `emitIntrinsicFromAST` loads *every* lvalue arg uniformly before dispatch — but `addrof`'s entire job is to return the *unloaded* address. `emitPointerIntrinsic`'s handler is just `return args[0]`, trusting args[0] is still an address by the time it gets there. It needs to skip the auto-load, the same way `scope_exit` already gets special-cased before the args loop even runs.

**`#toRef` doesn't do what your own Grammar doc says.** `Grammar.md` documents `#toRef(ptr) → non-null assertion + bitcast`. The implementation is just `return args[0]` with `// TODO: Add null check assertion`. Since `toRef` is supposed to be *the* safety boundary out of the sealed-conduit raw-pointer world, this is currently a silent passthrough — worth prioritizing over the other stubs since it's a safety hole, not just a missing feature.

**`#ptrOffset` computes bytes, not elements — also contradicts the spec.** `Grammar.md`: `#ptrOffset(p,n) → getelementptr inbounds <T>, ptr %p, i64 %n` (element-scaled GEP). The implementation casts to `i8*` and does `CreateGEP(i8Ty, ptr, offset)` — a raw byte offset regardless of `T`. For any pointee wider than 1 byte, `#ptrOffset(intPtr, 1)` moves 1 byte instead of 4. It also uses plain `CreateGEP` instead of `CreateInBoundsGEP`, dropping the `inbounds` the spec explicitly calls for.

**`pow` hardcodes a `double,double→double` libm signature regardless of actual operand type**, while `sqrt`/`abs`/`ceil`/etc. right below it correctly derive `argTypes` from the real LLVM value types. If Lucid `float` ever lowers to `f32` (plausible given `game.luc`'s Vec2/Vec3 math), `float ** float` passes f32 args into a function declared to take doubles — a verifier-level mismatch. And it's not defended from the caller side either: `CodeGenExpr.cpp`'s `BinaryOp::Pow` only promotes to double when *both* operands are integers, never when they're floats.

**Diagnostics lose their location.** `emitIntrinsic`'s "unknown intrinsic" error uses `SourceLocation()` (default) even though `expr->loc` is sitting right there in scope. The stub warnings in `emitAtomicIntrinsic`/`emitSIMDIntrinsic`/`emitStringIntrinsic`/`emitMemoryMgmtIntrinsic` can't report a location at all — they only take `ctx`, not `expr`. Since these are exactly the paths that'll keep firing while those categories are unimplemented, you'll get warnings pointing nowhere right when you need them most.

**Two sources of truth for intrinsic → LLVM-ID mapping.** `IntrinsicCallExprAST::intrinsicID` is set by Sema but never read here — `IntrinsicEmitter.cpp` re-derives the same mapping from the string name via its own if/else chains. If the two ever drift (new intrinsic added to Sema's registry, forgotten in `MATH_INTRINSICS`/etc. here), you get silent inconsistency instead of a compile error.

**Given your arena work specifically:** `emitMemoryMgmtIntrinsic` is a complete stub — `alloc`/`free`/`arena_create/alloc/reset/free` all just warn and return `nullptr`. `CodeGenContext` already has `runtimeFunctions`/`getRuntimeFunction`/`setRuntimeFunction` sitting there unused, clearly meant to hold the declare-once handles to your C++ runtime's `memory.cpp` functions — that's the wiring point when you're ready. And `#sizeof`/`#alignof` returning hardcoded `0` means any arena-sizing logic built on top of them will silently allocate zero bytes until that's filled in — worth sequencing before you try to test the arena descriptor end-to-end.

Yep — with both definitions and the header docs in front of me now, this confirms and sharpens what I flagged earlier.

## `loadIfNeeded` — two overloads, one is a dead stub, and the API shape is *why* the bug happened

```cpp
llvm::Value* loadIfNeeded(llvm::Value* value, llvm::Type* elemType, CodeGenContext& ctx);  // real
llvm::Value* loadIfNeeded(llvm::Value* value, bool isLValue, CodeGenContext& ctx);          // no-op
```

The `Type*` overload is the only one that actually calls `CreateLoad`. The `bool` overload just returns `value` unchanged — its own doc comment admits it ("Prefer the version with explicit elemType") and its body comment says "let the caller handle it," but nothing does. I already confirmed all 14 call sites in `CodeGenExpr.cpp` + `IntrinsicEmitter.cpp` use the broken `bool` one exclusively; the correct overload is never called anywhere in the files you've shown me.

Now that I can see both signatures side by side, I think I see *why*: `bool` is trivially available at every call site (`arg->isLValue` is just sitting on the AST node), while the `Type*` overload requires the caller to first resolve the pointee's LLVM type — which, with opaque pointers, means going back to `expr->resolvedType` and calling `getType(ctx, ...)`. That's real plumbing work. The easy overload is the broken one, so every caller reached for it. That's a classic "the API invites misuse" situation, not just 14 independent mistakes.

There's also a latent ambiguity landmine here: `bool` and `llvm::Type*` are both things `nullptr`/`0` can convert to. `loadIfNeeded(value, nullptr, ctx)` would be an ambiguous overload call and fail to compile — not hit yet since nobody calls the `Type*` version at all, but it's a sign these two shouldn't be overloads of the same name. I'd rename the bool one (`markLValue`/delete it) or give it a distinct name like `loadIfLValue` that forces callers to consciously pick, rather than letting overload resolution silently prefer whichever's easiest to call.

## `getMangledName` — doesn't account for generics, and `CodeGenContext` already has the data it's missing

```cpp
std::string getMangledName(const FuncDeclAST* decl, CodeGenContext& ctx) {
    // Simple mangling: just the name for now
    return ctx.pool.lookup(decl->name);
}
```

`CodeGenContext.hpp` already defines `GenericInstantiationKey` (`decl` + `typeArgs`) and a whole `GenericRegistry` keyed on exactly that — clearly built so two specializations of the same generic function get tracked separately. But `getMangledName` only takes a `FuncDeclAST*`, ignores type arguments entirely, and always returns the bare name. If this is what ultimately names the `llvm::Function` for each specialization (plausible, given `CodeGenGeneric.cpp` exists per your file tree), then `identity<int>` and `identity<float>` will both mangle to `"identity"` — either a duplicate-definition error from LLVM, or a silent overwrite of one specialization's IR by the other's declaration in `functionInstantiations`. Worth wiring `typeArgs` into the mangled name before generics get exercised for real, since the cache that would tell you *which* instantiation you're naming already exists right next to this function.

## Smaller notes, not bugs but worth knowing about

- **`createAlloca`** builds a fresh local `llvm::IRBuilder<> builder(ctx.llvmCtx)` rather than reusing `ctx.builder`, purely to reposition at the entry block without disturbing the caller's insertion point. That's the standard "hoist allocas to entry block" trick and is correct — just flagging so it doesn't look like an accidental second builder.
- **`emitPanic`** ends with `CreateUnreachable()`, which terminates the current basic block. Any caller that calls `emitPanic` and then keeps appending to `ctx.builder` in the same block afterward will silently insert dead/invalid instructions after a terminator. Not a bug in this function, but worth a one-line comment at the call sites (or an assertion) since it's an easy trap for whoever calls it later.
- **Doc/implementation mismatch**: `CodeGen.hpp`'s comment on `lowerIdentifierExpr` says it returns "loaded if r-value, pointer if l-value" — i.e. the function itself decides. But the actual calling convention used throughout `CodeGenExpr.cpp` is the opposite: every expression lowerer returns a raw pointer for lvalues and lets the *caller* decide whether to load via `loadIfNeeded`. Once `lowerIdentifierExpr` is implemented for real, whoever writes it should follow the pattern the rest of the file already uses, not the header comment — worth fixing the comment now so it doesn't mislead.