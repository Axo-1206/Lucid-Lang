# Lucid Core Scripts

> This document specifies the standard library of Lucid, written in Lucid itself. Every type, operator, function, and trait that a user program sees is declared here. The parser knows none of these names; it recognizes only the frames, punctuation, and boot-set keywords defined in the grammar. Everything else is content.
>
> **Load order.** Core scripts are loaded before any user code, in the order given by the module dependency graph. `core.luc` is the base module; every other core module imports it. User code sees the union of all core scripts that are transitively imported by `main.luc`.
>
> **Boot-set boundaries.** A core script may not introduce new keywords, new frame forms, or new syntax. It may only declare types, values, traits, DEFs, and host bindings using the frames the grammar already provides. The boot set is fixed (§Boot set, grammar); the core scripts fill it.
>
> **Host bindings.** Every operation that cannot be expressed in Lucid — memory, strings, fibers, host containers — is bound to a `#host`, `#native`, or `#builtin` target. The names in those bindings are the registry names that the runtime and the embedding engine provide. They are not user-visible.

---

## Table of Contents

- [Lucid Core Scripts](#lucid-core-scripts)
  - [Table of Contents](#table-of-contents)
  - [1. The Core Modules](#1-the-core-modules)
  - [2. `core.luc`](#2-coreluc)
    - [2.1 Primitive types](#21-primitive-types)
    - [2.2 Sized aliases](#22-sized-aliases)
    - [2.3 The OpKind registry](#23-the-opkind-registry)
    - [2.4 The boot traits](#24-the-boot-traits)
    - [2.5 Primitive operators](#25-primitive-operators)
      - [`int` (32-bit signed)](#int-32-bit-signed)
      - [`uint` (32-bit unsigned)](#uint-32-bit-unsigned)
      - [`float` (32-bit floating point)](#float-32-bit-floating-point)
      - [`bool`](#bool)
      - [`char`](#char)
      - [`string`](#string)
      - [Derived operators](#derived-operators)
    - [2.6 Derived operators](#26-derived-operators)
    - [2.7 Boolean operators](#27-boolean-operators)
    - [2.8 The nullable / fallible / combined wrappers](#28-the-nullable--fallible--combined-wrappers)
    - [2.9 `toStr`](#29-tostr)
      - [Primitive `toStr`](#primitive-tostr)
      - [Struct and enum fallbacks](#struct-and-enum-fallbacks)
      - [Container `toStr`](#container-tostr)
      - [The `Stringable` trait](#the-stringable-trait)
    - [2.10 Diagnostic functions](#210-diagnostic-functions)
    - [2.11 Memory intrinsics](#211-memory-intrinsics)
    - [2.12 Closure intrinsics](#212-closure-intrinsics)
    - [2.13 Reference and weak intrinsics](#213-reference-and-weak-intrinsics)
    - [2.14 Fiber intrinsics](#214-fiber-intrinsics)
    - [2.15 `Deferred<T>`](#215-deferredt)
    - [2.16 `Weak<T>`](#216-weakt)
    - [2.17 `scope_exit`](#217-scope_exit)
    - [2.18 Intrinsic DEFFs for `#builtin`](#218-intrinsic-deffs-for-builtin)
  - [3. `core.io.luc`](#3-coreioluc)
  - [4. `core.map.luc`](#4-coremapluc)
  - [5. `core.array.luc`](#5-corearrayluc)
  - [6. `core.string.luc`](#6-corestringluc)
  - [7. `core.math.luc`](#7-coremathluc)
  - [8. `core.fn.luc`](#8-corefnluc)
  - [9. `core.simd.luc`](#9-coresimdluc)
  - [Appendix A — Boot set vs. core scripts](#appendix-a--boot-set-vs-core-scripts)
  - [Appendix B — Host registry names](#appendix-b--host-registry-names)

---

## 1. The Core Modules

| Module        | Role                                                                                                                           | Imports |
| ------------- | ------------------------------------------------------------------------------------------------------------------------------ | ------- |
| `core`        | Primitives, operators, foundational types, `toStr`, `print`, `error`, `warn`, memory / closure / fiber / reference intrinsics. | —       |
| `core.io`     | Console and file I/O via the engine's VFS.                                                                                     | `core`  |
| `core.map`    | `Map<K, V>` and its operations.                                                                                                | `core`  |
| `core.array`  | Array operations and functional helpers.                                                                                       | `core`  |
| `core.string` | String manipulation.                                                                                                           | `core`  |
| `core.math`   | Arithmetic utilities, trigonometry, random.                                                                                    | `core`  |
| `core.fn`     | Function composition helpers.                                                                                                  | `core`  |
| `core.simd`   | SIMD type aliases (`Float4`, `Int4`, ...) and operations.                                                                      | `core`  |

Every core module except `core.luc` begins with `import core`. `core.luc` itself is the root: it declares the primitive types and the fundamental operations, and every other module builds on top of it.

---

## 2. `core.luc`

The base module. Every other module imports it; every user program sees its declarations.

### 2.1 Primitive types

Primitive types are bound to the runtime's native representations via `#host`. Each is a terminal binding: the type has no Lucid-side fields, no Lucid-side layout. Its layout is defined by the runtime ABI (`src/runtime-abi/lucid_abi.h`).

```lucid
-- Signed integers (machine-dependent sizes)
TYPE byte   = #host(int8)
TYPE short  = #host(int16)
TYPE int    = #host(int32)
TYPE long   = #host(int64)

-- Unsigned integers (machine-dependent sizes)
TYPE ubyte  = #host(uint8)
TYPE ushort = #host(uint16)
TYPE uint   = #host(uint32)
TYPE ulong  = #host(uint64)

-- Floating point
TYPE float  = #host(float)
TYPE double = #host(double)

-- Text and characters
TYPE char   = #host(char)
TYPE string = #host(string)

-- Boolean
TYPE bool   = #host(bool)

-- The unit type: the return type of every function that produces no value
TYPE unit   = #host(unit)
```

The names `int`/`uint` are aliases for the 32-bit variants on the reference platform. The size is a convention, not a guarantee of the language — a future core script could redefine the default width by declaring `TYPE int = #host(int64)`. The grammar itself does not know the names; it treats them as ordinary identifiers resolved against the core script's declarations.

### 2.2 Sized aliases

For code that needs a specific width, the sized aliases are declared here. They alias to the same host bindings but under a name that guarantees the width.

```lucid
TYPE int8   = byte
TYPE int16  = short
TYPE int32  = int
TYPE int64  = long

TYPE uint8  = ubyte
TYPE uint16 = ushort
TYPE uint32 = uint
TYPE uint64 = ulong

TYPE float32 = float
TYPE float64 = double
```

These are transparent aliases: `int32` and `int` are the same type on the reference platform. A value of type `int32` is a value of type `int`, and vice versa.

### 2.3 The OpKind registry

`OpKind` is the type of an operation category. The base module declares the standard kinds; the `DEF` frame resolves its `op_kind` slot against these values.

```lucid
TYPE OpKind = #host(OpKind)

FN registerOpKind (name string) -> OpKind = #host(register_op_kind)

const BINARY_OP  OpKind = registerOpKind("BINARY_OP")
const UNARY_OP   OpKind = registerOpKind("UNARY_OP")
const INDEX_GET  OpKind = registerOpKind("INDEX_GET")
const INDEX_SET  OpKind = registerOpKind("INDEX_SET")
const CALL       OpKind = registerOpKind("CALL")
```

The five standard kinds are the entire operation vocabulary. A `DEF` whose `op_kind` slot resolves to one of these values declares an operation of that category; the compiler's overload resolution dispatches on the pair `(op_kind, symbol)`.

- **`BINARY_OP`** — infix operations. The symbol is the operator's spelling: `'+'`, `'-'`, `'=='`, `'and'`, etc.
- **`UNARY_OP`** — prefix operations. The symbol is `'-'`, `'not'`, or `'~'`.
- **`INDEX_GET`** — read access on a container. No symbol; the container's type distinguishes the overload.
- **`INDEX_SET`** — write access on a container. No symbol.
- **`CALL`** — named-call operations. The symbol is the call's name: `'toStr'`, `'describe'`, etc.

### 2.4 The boot traits

Two traits are auto-satisfied by the compiler for every struct and every enum. They carry no clauses; they exist as markers so that generic `DEF`s can be written against them.

```lucid
trait StructType {}
trait EnumType   {}
```

The compiler inserts `satisfy StructType for X { }` for every `TYPE X = struct { ... }` declaration and `satisfy EnumType for X { }` for every `TYPE X = enum { ... }` declaration. These are the only `satisfy` blocks the compiler adds; every other `satisfy` block must be written in a core script or by user code.

The traits are used by `toStr`'s fallback family (§2.9) to dispatch on struct and enum types without needing to know them individually.

### 2.5 Primitive operators

Every operator on a primitive type is declared here as a `DEF`. The implementation is a `#native` binding — the interpreter has a fast path for these operations, so they don't go through a C function call.

#### `int` (32-bit signed)

```lucid
satisfy Eq for int {
    DEF BINARY_OP '==' (a int, b int) -> bool = #native(eq_i32)
}

satisfy Ord for int {
    DEF BINARY_OP '<' (a int, b int) -> bool = #native(lt_i32)
}

satisfy Add for int {
    DEF BINARY_OP '+' (a int, b int) -> int = #native(add_i32)
}

satisfy Sub for int {
    DEF BINARY_OP '-' (a int, b int) -> int = #native(sub_i32)
}

satisfy Mul for int {
    DEF BINARY_OP '*' (a int, b int) -> int = #native(mul_i32)
}

satisfy Div for int {
    DEF BINARY_OP '/' (a int, b int) -> int = #native(div_i32)
}

satisfy Rem for int {
    DEF BINARY_OP '%' (a int, b int) -> int = #native(rem_i32)
}

satisfy Neg for int {
    DEF UNARY_OP '-' (v int) -> int = #native(neg_i32)
}

satisfy Bitwise for int {
    DEF BINARY_OP '&'  (a int, b int) -> int = #native(bitand_i32)
    DEF BINARY_OP '|'  (a int, b int) -> int = #native(bitor_i32)
    DEF BINARY_OP '^'  (a int, b int) -> int = #native(bitxor_i32)
    DEF BINARY_OP '<<' (a int, b int) -> int = #native(shl_i32)
    DEF BINARY_OP '>>' (a int, b int) -> int = #native(shr_i32)
    DEF UNARY_OP  '~'  (v int) -> int = #native(bitnot_i32)
}
```

#### `uint` (32-bit unsigned)

```lucid
satisfy Eq for uint {
    DEF BINARY_OP '==' (a uint, b uint) -> bool = #native(eq_u32)
}

satisfy Ord for uint {
    DEF BINARY_OP '<' (a uint, b uint) -> bool = #native(lt_u32)
}

satisfy Add for uint {
    DEF BINARY_OP '+' (a uint, b uint) -> uint = #native(add_u32)
}

satisfy Sub for uint {
    DEF BINARY_OP '-' (a uint, b uint) -> uint = #native(sub_u32)
}

satisfy Mul for uint {
    DEF BINARY_OP '*' (a uint, b uint) -> uint = #native(mul_u32)
}

satisfy Div for uint {
    DEF BINARY_OP '/' (a uint, b uint) -> uint = #native(div_u32)
}

satisfy Rem for uint {
    DEF BINARY_OP '%' (a uint, b uint) -> uint = #native(rem_u32)
}

satisfy Bitwise for uint {
    DEF BINARY_OP '&'  (a uint, b uint) -> uint = #native(bitand_u32)
    DEF BINARY_OP '|'  (a uint, b uint) -> uint = #native(bitor_u32)
    DEF BINARY_OP '^'  (a uint, b uint) -> uint = #native(bitxor_u32)
    DEF BINARY_OP '<<' (a uint, b uint) -> uint = #native(shl_u32)
    DEF BINARY_OP '>>' (a uint, b uint) -> uint = #native(shr_u32)
    DEF UNARY_OP  '~'  (v uint) -> uint = #native(bitnot_u32)
}
```

#### `float` (32-bit floating point)

```lucid
satisfy Eq for float {
    DEF BINARY_OP '==' (a float, b float) -> bool = #native(eq_f32)
}

satisfy Ord for float {
    DEF BINARY_OP '<' (a float, b float) -> bool = #native(lt_f32)
}

satisfy Add for float {
    DEF BINARY_OP '+' (a float, b float) -> float = #native(add_f32)
}

satisfy Sub for float {
    DEF BINARY_OP '-' (a float, b float) -> float = #native(sub_f32)
}

satisfy Mul for float {
    DEF BINARY_OP '*' (a float, b float) -> float = #native(mul_f32)
}

satisfy Div for float {
    DEF BINARY_OP '/' (a float, b float) -> float = #native(div_f32)
}

satisfy Neg for float {
    DEF UNARY_OP '-' (v float) -> float = #native(neg_f32)
}
```

#### `bool`

```lucid
satisfy Eq for bool {
    DEF BINARY_OP '==' (a bool, b bool) -> bool = #native(eq_bool)
}

satisfy Add for bool {
    DEF BINARY_OP 'and' (a bool, b bool) -> bool = #native(and_bool)
}

satisfy Mul for bool {
    DEF BINARY_OP 'or' (a bool, b bool) -> bool = #native(or_bool)
}

DEF UNARY_OP 'not' (v bool) -> bool = #native(not_bool)
```

The choice to bind `and` to `Add` and `or` to `Mul` is a matter of convention — it lets the derived operators (§2.6) compose without special cases. `not` is a `UNARY_OP` with no trait wrapper, since it has no matching binary form.

#### `char`

```lucid
satisfy Eq for char {
    DEF BINARY_OP '==' (a char, b char) -> bool = #native(eq_char)
}

satisfy Ord for char {
    DEF BINARY_OP '<' (a char, b char) -> bool = #native(lt_char)
}
```

#### `string`

```lucid
satisfy Eq for string {
    DEF BINARY_OP '==' (a string, b string) -> bool = #builtin(str_eq)
}

satisfy Ord for string {
    DEF BINARY_OP '<' (a string, b string) -> bool = #builtin(str_lt)
}

satisfy Add for string {
    DEF BINARY_OP '+' (a string, b string) -> string = #builtin(str_concat)
}
```

Note that `string`'s `+` is concatenation, not arithmetic addition. The grammar does not distinguish; the `DEF` table does. `a + b` where both are `string` resolves to the concat `DEF`; where both are `int`, it resolves to the arithmetic `DEF`. This is the whole point of the `DEF`-based operator system.

#### Derived operators

The remaining comparison operators are derived generically, so they don't need per-type declarations:

```lucid
DEF BINARY_OP '!=' <T : Eq> (a T, b T) -> bool = { return not (a == b) }

DEF BINARY_OP '<=' <T : Ord> (a T, b T) -> bool = { return a < b or a == b }
DEF BINARY_OP '>'  <T : Ord> (a T, b T) -> bool = { return b < a }
DEF BINARY_OP '>=' <T : Ord> (a T, b T) -> bool = { return b <= a }
```

These generic `DEF`s apply to every type that satisfies `Eq` or `Ord`. A concrete `DEF` for a specific type takes precedence, per ordinary overload resolution.

### 2.6 Derived operators

The comparison operators `!=`, `<=`, `>`, `>=` are not declared per-type. They are declared once, generically, in terms of `Eq` and `Ord`:

```lucid
DEF BINARY_OP '!=' <T : Eq> (a T, b T) -> bool = {
    return not (a == b)
}

DEF BINARY_OP '<=' <T : Ord> (a T, b T) -> bool = {
    return a < b or a == b
}

DEF BINARY_OP '>' <T : Ord> (a T, b T) -> bool = {
    return b < a
}

DEF BINARY_OP '>=' <T : Ord> (a T, b T) -> bool = {
    return b <= a
}
```

A type that satisfies `Eq` automatically gets `!=`; a type that satisfies `Ord` gets `<=`, `>`, and `>=` as well (since `Ord : Eq`, it also gets `!=`). No per-type declaration is needed for these operators.

### 2.7 Boolean operators

The `and`, `or`, and `not` operators are declared on `bool` (§2.5). They are *short-circuiting* — the right operand is evaluated only if needed. This is a language-level property, not a `DEF`-table property; the compiler emits a short-circuit sequence for `and` and `or` regardless of the `DEF`'s implementation.

The truthiness rules for `if`, `while`, and the logical operators are defined by the grammar, not the core script. The core script's job is only to provide the `bool` result of the operation.

### 2.8 The nullable / fallible / combined wrappers

The wrapper structs for `T?`, `T!`, and `T?!` are declared here. They are ordinary structs from the compiler's point of view — the compiler just knows how to construct and destructure them, based on the `?`, `!`, and `?!` type suffixes.

```lucid
struct Option<T> {
    has   bool
    value T
}

struct Fallible<T> {
    ok    bool
    value T
}

struct Both<T> {
    tag   int   -- 0 = nil, 1 = value, 2 = err
    value T
}
```

The compiler's type checker treats `T?` as `Option<T>`, `T!` as `Fallible<T>`, and `T?!` as `Both<T>`. The wrappers are not user-facing; the user writes `T?` and the compiler wraps and unwraps automatically.

There are no `DEF`s on these wrappers in the core script. The compiler recognizes the nullable/fallible operations (`== nil`, `!= err`, `??`, narrowing) syntactically and emits the appropriate field reads and tag checks. They are not operations the `DEF` table resolves.

### 2.9 `toStr`

`toStr` is the stringification `CALL`-kind `DEF` family. It is declared once per type that has a custom or primitive string form, and there is a generic fallback for structs and enums.

#### Primitive `toStr`

```lucid
DEF CALL 'toStr' (v int)    -> string = #builtin(int_to_str)
DEF CALL 'toStr' (v uint)   -> string = #builtin(uint_to_str)
DEF CALL 'toStr' (v long)   -> string = #builtin(int_to_str)
DEF CALL 'toStr' (v ulong)  -> string = #builtin(uint_to_str)
DEF CALL 'toStr' (v float)  -> string = #builtin(float_to_str)
DEF CALL 'toStr' (v double) -> string = #builtin(float_to_str)
DEF CALL 'toStr' (v bool)   -> string = #builtin(bool_to_str)
DEF CALL 'toStr' (v char)   -> string = #builtin(char_to_str)
DEF CALL 'toStr' (v string) -> string = { return v }
```

#### Struct and enum fallbacks

```lucid
DEF CALL 'toStr' <T : StructType> (v T) -> string = #builtin(struct_to_str)
DEF CALL 'toStr' <T : EnumType>   (v T) -> string = #builtin(enum_to_str)
```

The struct fallback iterates the type's fields and formats each with `toStr`, producing `"Name { field: value, ... }"`. The enum fallback formats the variant as `"EnumName.VariantName"` for integer enums, or `"EnumName.Variant"` for payload enums. Neither inspects `T` at compile time — the compiler emits a per-instantiation body from the concrete type's field/variant list.

#### Container `toStr`

```lucid
DEF CALL 'toStr' <T> (v [*]T) -> string = {
    let s string = "["
    for i uint, x T in v {
        if i > 0 { s = s + ", " }
        s = s + toStr(x)
    }
    return s + "]"
}

DEF CALL 'toStr' <T> (v [_]T) -> string = {
    let s string = "["
    for i uint, x T in v {
        if i > 0 { s = s + ", " }
        s = s + toStr(x)
    }
    return s + "]"
}

DEF CALL 'toStr' <T> (d Deferred<T>) -> string = { return "<deferred>" }
DEF CALL 'toStr' <T> (w Weak<T>)     -> string = { return "<weak>" }
```

These use the recursive `toStr` dispatch; `toStr(x)` inside the loop resolves on `x`'s type, which may itself be a struct, enum, or another container.

#### The `Stringable` trait

```lucid
trait Stringable {
    REQUIRE CALL 'toStr' (self Self) -> string
}
```

A user type satisfies `Stringable` by declaring a `DEF CALL 'toStr'` for itself, most conveniently inside a `satisfy Stringable for X` block. The user's `DEF` overrides the struct or enum fallback by ordinary overload resolution. `Stringable` is a constraint, not a mechanism — `toStr` works on every type whether or not `Stringable` is ever mentioned.

### 2.10 Diagnostic functions

Two functions for user-facing diagnostics. `error(msg)` terminates the current fiber with a message and a stack trace; `warn(msg)` prints the message to the warning channel and returns.

```lucid
FN panic_str (msg string) = #builtin(panic_str)
FN warn_str  (msg string) = #builtin(warn_str)

const error (msg string) = { panic_str(msg) }
const warn  (msg string) = { warn_str(msg) }
```

`error` and `warn` are ordinary functions, not keywords. They are exported by `core` and available to user code.

### 2.11 Memory intrinsics

Memory operations that the compiler emits directly. These are `#builtin` because the compiler generates specialized code for them — usually inline, without a function call at all.

```lucid
FN sizeof<T>  () -> uint64 = #builtin(size_of)
FN alignof<T> () -> uint64 = #builtin(align_of)

FN alloc<T>   (count uint64) -> &T = #builtin(alloc)
FN free<T>    (p &T)               = #builtin(free)
FN realloc<T> (p &T, newCount uint64) -> &T = #builtin(realloc)

FN memcpy<T>  (dst &T, src &T, bytes uint64)      = #builtin(memcpy)
FN memmove<T> (dst &T, src &T, bytes uint64)      = #builtin(memmove)
FN memset<T>  (dst &T, value uint8, bytes uint64) = #builtin(memset)
```

`sizeof<T>()` and `alignof<T>()` resolve to compile-time constants — the compiler replaces them with a literal at the call site. `alloc<T>(n)` returns a managed reference to `n` values of type `T`, allocated from the runtime's allocator and tracked by the allocation registry. `free<T>(p)` releases it; double-free is caught by the registry.

`memcpy`, `memmove`, and `memset` are raw byte operations on references. They are the only memory operations that work directly on arbitrary byte ranges.

### 2.12 Closure intrinsics

The runtime operations that support closures. These are emitted by the compiler as part of closure construction and teardown; user code does not call them directly.

```lucid
FN fn_to_cls<A, B> (f fn (A) -> B) -> cls (A) -> B = #builtin(fn_to_cls)
FN call_fn<A, B>   (f fn (A) -> B, a A) -> B       = #builtin(call_fn)
FN call_cls<A, B>  (f cls (A) -> B, a A) -> B      = #builtin(call_cls)

FN alloc_env<T>     (size uint64) -> &T = #builtin(alloc_env)
FN retain_env<T>    (p &T)              = #builtin(retain_env)
FN release_env<T>   (p &T)              = #builtin(release_env)
```

`fn_to_cls` is the `fn → cls` coercion (§14.4 of the grammar). `call_fn` and `call_cls` are the two call protocols; the compiler picks the right one based on the function value's declared shape. `alloc_env`, `retain_env`, `release_env` manage the closure environment's refcount.

### 2.13 Reference and weak intrinsics

```lucid
FN weak<T>        (v &T)     -> Weak<T> = #builtin(weak)
FN upgrade<T>     (w Weak<T>) -> &T?    = #builtin(upgrade)
FN strongCount<T> (v &T)     -> uint    = #builtin(strong_count)
FN weakCount<T>   (w Weak<T>) -> uint   = #builtin(weak_count)
```

`weak(v)` constructs a weak reference from a strong one. `upgrade(w)` returns a strong reference if the referent is still alive, or `nil` if it has been freed. `strongCount` and `weakCount` are diagnostics — used to inspect the refcount state of a value.

### 2.14 Fiber intrinsics

The runtime operations that support concurrency. These are emitted by the compiler as part of `async`/`spawn`/`start`/`await`.

```lucid
FN spawn_fiber<T>   (f fn () -> T)                = #builtin(spawn_fiber)
FN start_fiber<T>   (f fn () -> T) -> Deferred<T> = #builtin(start_fiber)
FN await_deferred<T> (d &Deferred<T>) -> T        = #builtin(await_deferred)
FN cancel_deferred<T> (d &Deferred<T>)            = #builtin(cancel_deferred)
FN deferred_ready<T> (d &Deferred<T>) -> bool     = #builtin(deferred_ready)
```

`spawn_fiber` runs the function on a new fiber and discards the result. `start_fiber` runs it and returns a `Deferred<T>` handle. `await_deferred` suspends the current fiber until the deferred resolves. `cancel_deferred` requests cancellation. `deferred_ready` polls.

### 2.15 `Deferred<T>`

```lucid
TYPE Deferred<T> = #host(LucidDeferred)
```

`Deferred<T>` is a linear value: consumed exactly once by `await` or `cancel`. The compiler enforces this via the flow-sensitive linear-value check (§5.4 of the grammar). The runtime provides the handle; the compiler enforces the discipline.

The `cancel` function is bound to `cancel_deferred`, which consumes the deferred:

```lucid
const cancel<T> fn (d &Deferred<T>) = { cancel_deferred(d) }
```

`isReady` is a wrapper that doesn't consume:

```lucid
const isReady<T> fn (d &Deferred<T>) -> bool = { return deferred_ready(d) }
```

### 2.16 `Weak<T>`

```lucid
TYPE Weak<T> = #host(LucidWeak)
```

`Weak<T>` is a reference that does not increment the referent's refcount. It is the standard mechanism for breaking reference cycles. The `weak` and `upgrade` intrinsics (§2.13) are the only operations on it.

### 2.17 `scope_exit`

Registers a callback to run when the enclosing block exits. The registration is a compile-time operation — Sema populates the enclosing `BlockStmtAST::scopeExits`, and CodeGen emits the callbacks at every exit edge.

```lucid
FN scope_exit<T> (f cls (T) -> unit, v T) = #builtin(scope_exit)
```

The callback takes exactly one argument of type `T`. Passing more than one value is done by wrapping them in a struct. Multiple `scope_exit` calls in the same block run in LIFO order.

### 2.18 Intrinsic DEFFs for `#builtin`

Some `#builtin` handlers are declared as `DEF`s rather than `FN`s because they participate in overload resolution. These are the `OpKind` handlers the compiler emits for primitive operations on types the core script doesn't declare itself. In practice, all primitive operations in this module are declared via `satisfy` blocks (§2.5), so no extra `DEF`s are needed here.

The `#builtin` names referenced throughout this module (`int_to_str`, `struct_to_str`, `alloc`, `weak`, ...) are not declared anywhere in the core script — they are compiler-recognized names, listed in the `#builtin` registry (grammar §17). The core script only declares the *bindings*: `FN sizeof<T>() = #builtin(size_of)` and so on.

---

## 3. `core.io.luc`

Console and file I/O, routed through the engine's VFS.

```lucid
import core

-- Console output
FN write    (s string) = #host(host_write)
FN writeErr (s string) = #host(host_write_err)

const print<T>    fn (v T) = { write(toStr(v)) }
const println<T>  fn (v T) = { write(toStr(v)); write("\n") }
const printErr<T> fn (v T) = { writeErr(toStr(v)) }
const printlnErr<T> fn (v T) = { writeErr(toStr(v)); writeErr("\n") }

-- Console input
FN readLine () -> string = #host(host_read_line)

-- File I/O via the engine VFS
FN readFile   (path string) -> string! = #host(host_read_file)
FN writeFile  (path string, contents string) = #host(host_write_file)
FN fileExists (path string) -> bool = #host(host_file_exists)
```

`print` and `println` are generic over `T`; they call `toStr` on their argument, which dispatches through the `CALL 'toStr'` table. There is no variadic form — to print several values, concatenate with `+` and `toStr`, or use string interpolation.

File I/O is routed through the engine's VFS, so paths resolve relative to the game's content root, not the OS filesystem. `readFile` returns a fallible string, since reading can fail.

---

## 4. `core.map.luc`

The `Map<K, V>` type and its operations. `Map` is a host-backed type; its storage is on the C++ side.

```lucid
import core

TYPE Map<K, V> = #host(LucidMap)

FN map_new<K, V>    () -> Map<K, V>          = #host(map_new)
FN map_len<K, V>    (m &Map<K, V>)           -> uint = #host(map_len)
FN map_has<K, V>    (m &Map<K, V>, k K)      -> bool = #host(map_has)
FN map_remove<K, V> (m &Map<K, V>, k K)      -> bool = #host(map_remove)
FN map_keys<K, V>   (m &Map<K, V>)           -> [*]K = #host(map_keys)
FN map_values<K, V> (m &Map<K, V>)           -> [*]V = #host(map_values)

DEF INDEX_GET (m &Map<K, V>, k K) -> V? = #host(map_get)
DEF INDEX_SET (m &Map<K, V>, k K, v V)  = #host(map_set)
```

`Map` has no literal form. Construction is via `map_new`, and entries are added with `m[k] = v`.

**Example:**

```lucid
let scores Map<string, int> = map_new<string, int>()
scores["alice"] = 10
scores["bob"]   = 20

for k string, v int in scores {
    println(k + ": " + toStr(v))
}
```

The `for` loop's first binding is the key type `K`; the second is the value type `V`. Iterating keys or values only is done with `_`:

```lucid
for k string, _ in scores { println(k) }
for _, v int in scores    { println(toStr(v)) }
```

**A helper for building from pairs:**

```lucid
struct Entry<K, V> {
    key   K
    value V
}

const map_of<K, V> fn (entries [_]Entry<K, V>) -> Map<K, V> = {
    let m Map<K, V> = map_new<K, V>()
    for _, e Entry<K, V> in entries {
        m[e.key] = e.value
    }
    return m
}
```

Usage:

```lucid
let scores Map<string, int> = map_of<string, int>([
    Entry<string, int> { key = "alice", value = 10 }
    Entry<string, int> { key = "bob",   value = 20 }
])
```

---

## 5. `core.array.luc`

Array operations. The arrays themselves (`[*]T`, `[_]T`, `[N]T`) are boot-level type forms; the operations are free functions.

```lucid
import core

FN array_len<T>    (a [_]T)                 -> uint = #builtin(array_len)
FN array_push<T>   (a &[*]T, v T)                  = #builtin(array_push)
FN array_pop<T>    (a &[*]T)                -> T?   = #builtin(array_pop)
FN array_insert<T> (a &[*]T, i uint, v T)          = #builtin(array_insert)
FN array_remove<T> (a &[*]T, i uint)               = #builtin(array_remove)
FN array_resize<T> (a &[*]T, n uint)               = #builtin(array_resize)
FN array_clear<T>  (a &[*]T)                       = #builtin(array_clear)
```

`array_pop` returns `T?` because the array might be empty. The other operations are void.

**Functional helpers:**

```lucid
const array_map<T, U> fn (xs [_]T, f cls (T) -> U) -> [*]U = {
    let result [*]U = []
    for _, x T in xs {
        array_push(result, f(x))
    }
    return result
}

const array_filter<T> fn (xs [_]T, pred cls (T) -> bool) -> [*]T = {
    let result [*]T = []
    for _, x T in xs {
        if pred(x) {
            array_push(result, x)
        }
    }
    return result
}

const array_reduce<T, U> fn (xs [_]T, seed U, f cls (U, T) -> U) -> U = {
    let acc U = seed
    for _, x T in xs {
        acc = f(acc, x)
    }
    return acc
}

const array_find<T> fn (xs [_]T, pred cls (T) -> bool) -> T? = {
    for _, x T in xs {
        if pred(x) { return x }
    }
    return nil
}

const array_any<T> fn (xs [_]T, pred cls (T) -> bool) -> bool = {
    for _, x T in xs {
        if pred(x) { return true }
    }
    return false
}

const array_all<T> fn (xs [_]T, pred cls (T) -> bool) -> bool = {
    for _, x T in xs {
        if not pred(x) { return false }
    }
    return true
}
```

**`array_sort`:**

```lucid
const array_sort<T> fn (xs [*]T, cmp fn (T, T) -> int) = {
    -- in-place sort, calling cmp
}
```

The comparator is `fn` (not `cls`) — sort is a hot path, and the comparator should not capture. It returns a negative value if the first argument sorts before the second, zero if equal, and a positive value otherwise.

**Usage:**

```lucid
let xs [*]int = [3, 1, 4, 1, 5]

array_sort(xs, (a int, b int) -> int { return a - b })

let doubled [*]int = array_map<int, int>(xs, (x int) -> int { return x * 2 })
let evens   [*]int = array_filter<int>(xs, (x int) -> bool { return x % 2 == 0 })
```

The `map`/`filter`/`reduce` callbacks are `cls` (allowing captures); the `sort` comparator is `fn` (no captures, hot path).

---

## 6. `core.string.luc`

String manipulation.

```lucid
import core

FN strLen     (s string) -> uint = #builtin(str_len)
FN strEq      (a string, b string) -> bool = #builtin(str_eq)
FN strConcat  (a string, b string) -> string = #builtin(str_concat)
FN strSlice   (s string, from uint, to uint) -> string = #builtin(str_slice)
FN strFromPtr (p &uint8, len uint) -> string = #builtin(str_from_ptr)

const split fn (s string, sep string) -> [*]string = { ... }
const trim  fn (s string) -> string = { ... }
const find  fn (s string, needle string) -> uint? = { ... }
const contains fn (s string, needle string) -> bool = { ... }
const startsWith fn (s string, prefix string) -> bool = { ... }
const endsWith   fn (s string, suffix string) -> bool = { ... }
const replace    fn (s string, from string, to string) -> string = { ... }
```

**String conversion functions:**

```lucid
const stringFromInt   fn (n int)   -> string = { return toStr(n) }
const stringFromFloat fn (f float) -> string = { return toStr(f) }
const stringFromBool  fn (b bool)  -> string = { return toStr(b) }

const intFromString   fn (s string) -> int!   = { ... }
const floatFromString fn (s string) -> float! = { ... }
```

`intFromString` and `floatFromString` are fallible — parsing can fail. The returned `int!` must be narrowed before use:

```lucid
let n int! = intFromString("42")
let v int = n ?? 0
```

**The `+` operator on strings** is bound in `core.luc` (§2.5) to `str_concat`. So `"a" + "b"` is equivalent to `strConcat("a", "b")`, and both produce `"ab"`.

---

## 7. `core.math.luc`

Arithmetic utilities, trigonometry, and random number generation.

```lucid
import core

FN sqrt  (x float) -> float = #native(sqrt_f32)
FN pow   (base float, exp float) -> float = #native(pow_f32)
FN sin   (x float) -> float = #native(sin_f32)
FN cos   (x float) -> float = #native(cos_f32)
FN tan   (x float) -> float = #native(tan_f32)
FN floor (x float) -> float = #native(floor_f32)
FN ceil  (x float) -> float = #native(ceil_f32)
FN round (x float) -> float = #native(round_f32)
FN abs   (x float) -> float = #native(abs_f32)

const min<T : Ord> fn (a T, b T) -> T = { return if a < b ?? a else b }
const max<T : Ord> fn (a T, b T) -> T = { return if a > b ?? a else b }

const PI  float = 3.141592653589793
const TAU float = 6.283185307179586
const E   float = 2.718281828459045

FN random     () -> float = #host(host_random)
FN randomInt  (lo int, hi int) -> int = #host(host_random_int)
FN seedRandom (seed uint64) = #host(host_seed_random)
```

**Usage:**

```lucid
import core.math as math

let r float = math::sqrt(2.0)
let angle float = math::PI / 4.0
let s float = math::sin(angle)
```

---

## 8. `core.fn.luc`

Function composition helpers.

```lucid
import core

const identity<T> fn (v T) -> T = { return v }

const constFn<T, U> fn (v T) -> fn (U) -> T = {
    return (x U) -> T { return v }
}

const compose2<A, B, C> fn (f cls (A) -> B, g cls (B) -> C) -> cls (A) -> C = {
    return (x A) -> C { return g(f(x)) }
}

const compose3<A, B, C, D> fn (f cls (A) -> B, g cls (B) -> C, h cls (C) -> D) -> cls (A) -> D = {
    return (x A) -> D { return h(g(f(x))) }
}
```

All parameters and returns are `cls`. The parameters are `cls` for permissiveness — a `fn` argument coerces up, and composition isn't a hot path. The return type is `cls` out of necessity — the composed function captures `f`, `g`, and `h` in its environment.

**Usage:**

```lucid
import core.fn as fn

const process fn (raw string) -> bool =
    fn::compose3(validate, transform, render)
```

The pipeline form is preferred for inline composition; `compose2`/`compose3` are for named reusable function values.

---

## 9. `core.simd.luc`

SIMD type aliases and operations. `Simd<T, N>` is a compiler-builtin type constructor; the named aliases (`Float4`, `Int4`, ...) are the user-facing interface.

```lucid
import core

TYPE Simd<T, N> = #builtin(simd_type)

-- Named SIMD types for the common widths
TYPE Float2 = Simd<float, 2>
TYPE Float4 = Simd<float, 4>
TYPE Float8 = Simd<float, 8>

TYPE Int2  = Simd<int, 2>
TYPE Int4  = Simd<int, 4>
TYPE Int8  = Simd<int, 8>

TYPE UInt4 = Simd<uint, 4>
TYPE UInt8 = Simd<uint, 8>
```

**Operations for `Float4`:**

```lucid
FN float4_splat   (v float) -> Float4 = #builtin(simd_splat)
FN float4_add     (a Float4, b Float4) -> Float4 = #builtin(simd_add)
FN float4_sub     (a Float4, b Float4) -> Float4 = #builtin(simd_sub)
FN float4_mul     (a Float4, b Float4) -> Float4 = #builtin(simd_mul)
FN float4_div     (a Float4, b Float4) -> Float4 = #builtin(simd_div)
FN float4_load    (p &float) -> Float4 = #builtin(simd_load)
FN float4_store   (p &float, v Float4) = #builtin(simd_store)
FN float4_extract (v Float4, i uint) -> float = #builtin(simd_extract)
```

Operations for `Float8`, `Int4`, and the others follow the same pattern. The `#builtin(simd_*)` handlers validate `T` and `N` and lower to LLVM vector types (or, under the interpreter, to the runtime's SIMD helpers).

**Usage:**

```lucid
let a Float4 = float4_splat(1.0)
let b Float4 = float4_splat(2.0)
let c Float4 = float4_add(a, b)
```

The generic `Simd<T, N>` form is available for widths not covered by the named types. The named types are the recommended interface; the generic form is for advanced use.

---

## Appendix A — Boot set vs. core scripts

The following are **boot-set** (parser-recognized, not declared anywhere):

- Frame keywords: `TYPE`, `FN`, `const`, `let`, `trait`, `satisfy`, `DEF`, `REQUIRE`, `import`.
- Content markers: `struct`, `enum`, `fn`, `cls`, `as`, `Self`.
- Statement keywords: `if`, `else`, `for`, `while`, `do`, `switch`, `case`, `default`, `break`, `continue`, `return`.
- Concurrency keywords: `async`, `spawn`, `start`, `await`, `all`, `any`.
- Punctuation and operators.
- Literals: `nil`, `err`, `true`, `false`, numeric / string / char literals.
- Target markers: `#host`, `#native`, `#builtin`.
- Attribute names: `export`, `foreign`, `link`, `deprecated`, `inline`, `noinline`, `opaque`, `host_only`.

The following are **core-script** (declared in `core.luc` or another core module, not in the parser):

- Every primitive type name: `int`, `float`, `bool`, `string`, `char`, `byte`, and their sized variants.
- Every `OpKind` name: `BINARY_OP`, `UNARY_OP`, `INDEX_GET`, `INDEX_SET`, `CALL`.
- Every trait: `Eq`, `Ord`, `Add`, `Sub`, `Mul`, `Div`, `Rem`, `Neg`, `Bitwise`, `Numeric`, `Integral`, `Stringable`, `StructType`, `EnumType`.
- Every operator's meaning for a given type: `+` on `int`, `+` on `string`, `==` on `Vec2`, etc.
- Every function: `print`, `println`, `toStr`, `sizeof`, `alloc`, `weak`, `upgrade`, `scope_exit`, `error`, `warn`, `map_new`, etc.
- Every host-backed type: `Map`, `Deferred`, `Weak`, `OpKind`, `Simd`, and the primitive type host bindings.

The dividing line: the parser recognizes **frames, markers, punctuation, and boot keywords**. Everything else is a declaration in a core script, resolved by the same name lookup that resolves user declarations.

---

## Appendix B — Host registry names

The names in `#host(...)`, `#native(...)`, and `#builtin(...)` bindings are not Lucid identifiers. They are runtime-registry names. The full registry is defined by the runtime (`src/runtime-abi/functions.def` for the ABI surface, `src/host/Registry.hpp` for the embedder's additions).

The core scripts reference three categories:

**`#host(name)`** — a C++ function or type registered by the runtime at startup. These are the runtime's own host bindings: `map_new`, `map_get`, `map_set`, `host_write`, `host_read_line`, `host_read_file`, `host_write_file`, `host_random`, `LucidMap`, `LucidDeferred`, `LucidWeak`, `OpKind`, etc.

**`#native(name)`** — a runtime opcode. These are the primitive operations the interpreter has fast paths for: `add_i32`, `sub_i32`, `mul_i32`, `div_i32`, `eq_i32`, `lt_i32`, `sqrt_f32`, `pow_f32`, `sin_f32`, etc. The interpreter dispatches these directly; there is no function call.

**`#builtin(name)`** — a compiler-emitted operation. These are operations the compiler handles specially, usually by emitting specialized code per instantiation: `size_of`, `align_of`, `alloc`, `free`, `memcpy`, `int_to_str`, `struct_to_str`, `enum_to_str`, `weak`, `upgrade`, `simd_type`, `simd_add`, `scope_exit`, etc.

The set of `#host` names is closed at runtime-start by the runtime's registration. The set of `#native` and `#builtin` names is closed at compiler-build by the compiler's handler registry. A core script cannot add new names in either category; only the runtime and the compiler can.

The engine that embeds Lucid can register its own host functions and types (see grammar §66, the Foreign Function Interface). Those go into the same `#host` namespace, resolved by the same registry lookup. From the core script's perspective, they are indistinguishable from the runtime's own bindings.