Here's the rewritten **Compiler Intrinsics** section and related updates to use `#` for intrinsics and keep `@` for attributes:

---

## `@` and `#` — Compiler Directives

Two distinct prefixes, distinguished by position and symbol:

| Prefix | Position | Purpose |
|--------|----------|---------|
| `@name` / `@name(args)` | Before a declaration | Attach metadata (attribute) |
| `#name(args)` | In expression position | Compiler-builtin call (intrinsic) |

### Attributes (`@`)

```
attribute       := '@' IDENTIFIER [ '(' attr_arg_list ')' ]

attr_arg_list   := attr_arg { ',' attr_arg }

attr_arg        := STRING_LITERAL | INT_LITERAL | HEX_LITERAL | 'true' | 'false' | IDENTIFIER
```

Attribute arguments are intentionally limited to compile-time literals and
type identifiers. Runtime expressions are not valid inside attribute arguments.

#### Known Attributes

| Attribute | Valid on | Purpose |
|---|---|---|
| `@extern("sym")` | `let`, `const` func/var | Bind to C/OS/Vulkan symbol |
| `@extern("sym", "conv")` | `let`, `const` func/var | With explicit calling convention |
| `@inline` | func | Suggest always inline |
| `@noinline` | func | Prevent inlining |
| `@packed` | `struct` | Remove padding — all fields byte-adjacent |
| `@deprecated("msg")` | func, var, struct | Emit warning at every use site |
| `@aot` | `main` only | Ahead-of-time compilation |
| `@jit` | `main` only | JIT compilation |

`@inline` and `@noinline` are mutually exclusive on the same declaration.
`@aot` and `@jit` are mutually exclusive on the same declaration.

#### `@extern` Rules

- Requires `const`, not `let` — the linker resolves the symbol permanently
- Functions must have no body
- `*T` raw pointer is only valid on `@extern`-decorated declarations
- `W3001` warning when `let` is used instead of `const`
- `W3002` warning when an empty body `= {}` is supplied (body silently ignored)
- `E3002` error when a non-empty body is supplied

```luc
@extern("malloc")
const malloc (size uint64) -> *uint8?

@extern("free")
const free (ptr *uint8)

@extern("printf", "C")
const printf (fmt *uint8, args ...any) -> int

@extern("vkCreateInstance")
const vkCreateInstance (
    pInfo      *VkInstanceCreateInfo,
    pAllocator *VkAllocationCallbacks,
    pInstance  **VkInstance
) -> uint32

@extern("__stack_top")
const stackTop *uint8
```

---

### Compiler Intrinsics (`#`)

Intrinsic calls appear in expression position and are prefixed with `#`.

```
intrinsic_call  := '#' IDENTIFIER '(' [ intrinsic_arg_list ] ')'

intrinsic_arg_list := intrinsic_arg { ',' intrinsic_arg }

intrinsic_arg   := expr
                 | type
```

Unlike attributes, intrinsics can take runtime expressions and types as arguments.

---

#### Compile-time type queries

| Intrinsic | Returns | Notes |
|---|---|---|
| `#sizeof(T)` | `uint64` | Byte size of type T — compile-time constant |
| `#alignof(T)` | `uint64` | Alignment requirement of T — compile-time constant |

```luc
let size   uint64 = #sizeof(Vertex)
let align  uint64 = #alignof(Vec2)
```

---

#### Floating-point math

| Intrinsic | Args | Returns | Notes |
|---|---|---|---|
| `#sqrt(x)` | float/double | same | Hardware square root |
| `#floor(x)` | float/double | same | Round toward −∞ |
| `#ceil(x)` | float/double | same | Round toward +∞ |
| `#round(x)` | float/double | same | Round to nearest, half away from zero |
| `#abs(x)` | numeric | same | Absolute value |
| `#pow(base, exp)` | float/double | same | Exponentiation |
| `#fma(a, b, c)` | float/double | same | Fused multiply-add: `(a*b)+c` |
| `#min(a, b)` | same type | same | Minimum |
| `#max(a, b)` | same type | same | Maximum |

```luc
let hyp       float = #sqrt(x*x + y*y)
let rounded   float = #round(value)
let maxVal    int   = #min(a, b)
```

---

#### Bit manipulation (integer types only)

| Intrinsic | Args | Returns | Notes |
|---|---|---|---|
| `#clz(x)` | integer | same | Count leading zero bits |
| `#ctz(x)` | integer | same | Count trailing zero bits |
| `#popcount(x)` | integer | same | Count set (1) bits |
| `#bswap(x)` | integer | same | Reverse byte order (endianness) |

```luc
let leading   uint32 = #clz(flags)
let trailing  uint32 = #ctz(flags)
let bits      uint32 = #popcount(mask)
let swapped   uint32 = #bswap(networkOrder)
```

---

#### Memory operations

| Intrinsic | Args | Returns | Notes |
|---|---|---|---|
| `#memcpy(dst, src, len)` | ptr, ptr, uint64 | void | Copy bytes, no overlap |
| `#memmove(dst, src, len)` | ptr, ptr, uint64 | void | Copy bytes, handles overlap |
| `#memset(dst, val, len)` | ptr, ubyte, uint64 | void | Fill bytes with value |

All memory intrinsics operate on raw pointers (`*T`) and are only valid inside
`@extern`-decorated functions or other intrinsic calls.

```luc
#memcpy(dest, src, #sizeof(Buffer))
#memset(ptr, 0, size)
```

---

#### Pointer operations (The Sealed Conduit boundary)

| Intrinsic | Args | Returns | Notes |
|---|---|---|---|
| `#ptrToRef(T, ptr)` | type, `*T` | `&T` | Assert valid, cross to safe reference |
| `#refToPtr(ref)` | `&T` | `*T` | Convert reference to raw pointer |
| `#ptrOffset(ptr, n)` | `*T`, int | `*T` | Pointer arithmetic (element offset) |
| `#ptrDiff(p1, p2)` | `*T`, `*T` | `int64` | Distance between pointers in elements |

These intrinsics are the only way to cross the sealed conduit boundary or
perform pointer arithmetic.

```luc
let buf *uint8 = malloc(1024)
let ref &uint8 = #ptrToRef(&uint8, buf)
ref = 0xFF

let next *uint8 = #ptrOffset(buf, 1)
let distance int64 = #ptrDiff(next, buf)
```

---

#### Unsafe / Bit reinterpretation

| Intrinsic | Args | Returns | Notes |
|---|---|---|---|
| `#bitcast(T, x)` | type, value | `T` | Reinterpret bits of x as type T; sizes must match |

Valid only inside `@extern`-decorated functions or when the compiler flag
`--unsafe` is enabled.

```luc
let bits uint32 = 0x3F800000
let f   float32 = #bitcast(float32, bits)   -- 1.0
```

---

### Summary: `@` vs `#`

| Feature | `@` (Attribute) | `#` (Intrinsic) |
|---------|----------------|-----------------|
| Position | Before declaration | In expression |
| Args | Compile-time literals only | Runtime expressions / types |
| Evaluation | Parsed, stored, used by compiler | Inlined / replaced by compiler |
| User-defined | No | No |
| Example | `@extern("malloc")` | `#sizeof(T)` |

---

## Updated examples in other sections

### From `@extern` example (unchanged — still uses `@`):

```luc
@extern("malloc")
const malloc (size uint64) -> *uint8?

@extern("free")
const free (ptr *uint8)
```

### From `@ptrToRef` (now `#ptrToRef`):

```luc
let buf *uint8? = malloc(1024)
if buf == nil { return 1 }

let ref &uint8 = #ptrToRef(&uint8, buf)    -- cross the boundary
ref = 0xFF

let next *uint8? = #ptrOffset(buf, 1)      -- pointer arithmetic
```

### From `@sqrt` (now `#sqrt`):

```luc
impl Vec2 {
    length () -> float = {
        return #sqrt(x*x + y*y)
    }
}
```

### From `@bitcast` (now `#bitcast`):

```luc
let bits uint32 = 0x3F800000
let f   float32 = #bitcast(float32, bits)
```