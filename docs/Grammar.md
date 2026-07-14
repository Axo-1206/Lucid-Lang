# Lucid — Grammar Reference

> Lucid is the successor to Luc. It keeps Luc's core ideas — primitive types,
> function types, currying, generics, pipeline, composition, result-based error
> handling, and foreign-function communication — while removing `impl`, `from`,
> traits, and method dispatch entirely. All behavior is expressed as plain
> functions.

> [!NOTE]
> the language was design to be visual but the design was impractical
> so the visual design was discarded. A better visual design for the
> game engine will be made as a replacement

---

## Execution Model

Lucid has **one language and one syntax** regardless of how it is run. The same
source file executes under the interpreter today and will compile natively in the
future — nothing in the source changes between modes.

**Current:** Lucid ships as an interpreter. Run a program with:

```
lucid run main.lucid
```

**Future:** A native ahead-of-time compiler will be added later. The invocation
will be:

```
lucid build main.lucid
```

The execution mode is a **toolchain concern, not a source concern.** There are no
attributes, pragmas, or annotations in the source that select between interpreted
and compiled execution. The syntax is strict in both modes — interpreted Lucid is
not a relaxed or scripting subset of compiled Lucid. They are identical.

> [!NOTE]
> Lucid's strict syntax is intentional even for the interpreter. It keeps
> interpreter behavior predictable, makes the eventual compiler's job
> straightforward, and means there is no dialect gap to close later.

---

## Implementation Notes

This section documents the intended architecture of the Lucid compiler.
It is not part of the language specification — source code that conforms to the
grammar is valid Lucid regardless of how the compiler implements it internally.
This section exists to record design intent and guide the implementation.

---

### LLVM as the Shared Backend

Both the interpreter (`lucid run`) and the future compiler (`lucid build`) are
built on **LLVM**. The Lucid frontend — parser, type checker, and code lowering
— produces LLVM IR. What happens to that IR is the only difference between the
two modes:

```
[Lucid source]
      │
      ▼
[Lucid frontend]
  parser · type checker · IR lowering
      │
      ▼
[LLVM IR]  ← single shared representation
      │
      ├─────────────────────┐
      ▼                     ▼
[LLVM AOT]             [LLVM ORC JIT]
emit object file       compile in memory
system linker          execute immediately
`lucid build`          `lucid run`
```

This architecture means the frontend is written exactly once. The compiler and
interpreter share every stage up to and including IR generation. The split at the
bottom is small — roughly 100 lines each for the AOT emission path and the JIT
execution path.

---

### Intrinsics — Written Once, Used by Both

Every Lucid intrinsic (`#sqrt`, `#atomic_add`, `#memcpy`, etc.) maps directly to
an LLVM intrinsic or LLVM IR instruction. The lowering is written once in the
frontend and produces the same IR node regardless of whether the IR is later
compiled AOT or executed by the JIT.

```
Lucid intrinsic  →  LLVM IR node
─────────────────────────────────────────────────────
#sqrt(x)         →  call @llvm.sqrt.f32(float %x)
#abs(x)          →  call @llvm.fabs.f32(float %x)
#fma(a, b, c)    →  call @llvm.fma.f32(float %a, %b, %c)
#clz(x)          →  call @llvm.ctlz.i32(i32 %x, i1 false)
#ctz(x)          →  call @llvm.cttz.i32(i32 %x, i1 false)
#popcount(x)     →  call @llvm.ctpop.i32(i32 %x)
#bswap(x)        →  call @llvm.bswap.i32(i32 %x)
#memcpy(d,s,n)   →  call @llvm.memcpy(ptr %d, ptr %s, i64 %n, ...)
#memmove(d,s,n)  →  call @llvm.memmove(ptr %d, ptr %s, i64 %n, ...)
#memset(d,v,n)   →  call @llvm.memset(ptr %d, i8 %v, i64 %n, ...)
#prefetch(ptr)   →  call @llvm.prefetch(ptr %p, i32 0, i32 3, i32 1)
#fence(ord)      →  fence <ordering>
#pause()         →  call @llvm.x86.sse2.pause()
#likely(expr)    →  branch weight metadata on the conditional
#unlikely(expr)  →  branch weight metadata on the conditional
#atomic_load     →  load atomic <type> <ptr> <ordering>
#atomic_store    →  store atomic <type> <val>, <ptr> <ordering>
#atomic_add      →  atomicrmw add <ptr>, <val> <ordering>
#atomic_sub      →  atomicrmw sub <ptr>, <val> <ordering>
#atomic_and      →  atomicrmw and <ptr>, <val> <ordering>
#atomic_or       →  atomicrmw or  <ptr>, <val> <ordering>
#atomic_xor      →  atomicrmw xor <ptr>, <val> <ordering>
#atomic_cas      →  cmpxchg <ptr>, <exp>, <val> <ordering>
#simd_add        →  add <N x T> %a, %b
#simd_mul        →  mul <N x T> %a, %b
#simd_load       →  load <N x T>, ptr %p
#simd_store      →  store <N x T> %v, ptr %p
#sizeof(T)       →  getelementptr / DataLayout::getTypeAllocSize (compile-time)
#alignof(T)      →  DataLayout::getABITypeAlignment (compile-time)
#bitcast(T, x)   →  bitcast <src_type> %x to <dst_type>
#addrof(x)       →  IR value's pointer (no extra instruction)
#toRef(ptr)      →  non-null assertion + bitcast
#toPtr(ref)      →  bitcast to pointer type
#ptrOffset(p,n)  →  getelementptr inbounds <T>, ptr %p, i64 %n
#ptrDiff(p1,p2)  →  sub (ptrtoint %p1), (ptrtoint %p2)
```

The intrinsic lowering code is a single module in the frontend — not split
between the interpreter and compiler paths. Both paths inherit it by consuming
the same IR.

---

### Foreign Function Resolution

Foreign symbols declared with `@[foreign("C")]` are resolved differently in each
mode, but both use LLVM's machinery:

**Compiler mode (`lucid build`)** — the Lucid compiler emits object files and
passes `@[link(...)]` names and paths directly to the system linker (`ld`, `lld`,
`link.exe`). Foreign symbols are resolved at link time. Static (`.a`) and dynamic
(`.so`/`.dylib`/`.dll`) libraries are both supported.

**Interpreter mode (`lucid run`)** — the ORC JIT has a built-in dynamic linker.
Foreign shared libraries named in `@[link(...)]` are loaded via `dlopen` (Linux/
macOS) or `LoadLibrary` (Windows) at startup, and their symbols are registered
with the JIT's symbol table. When the JIT encounters a call to a foreign function,
it resolves the symbol from the registered libraries — the same resolution that
the system linker would perform at compile time, but done in memory at runtime.

The practical implication for `@[foreign("C")]` users:

| Scenario                            | Compiler mode                    | Interpreter mode                       |
| ----------------------------------- | -------------------------------- | -------------------------------------- |
| System library (`"m"`, `"pthread"`) | `-lm` passed to linker           | `libm.so` loaded via `dlopen`          |
| Source file (`"wrapper.c"`)         | compiled and linked by the build | must be pre-compiled to `.so`/`.dylib` |
| Object file (`"wrapper.o"`)         | linked directly                  | must be wrapped in a shared library    |
| Shared library (`"libfoo.so"`)      | linked dynamically               | loaded via `dlopen`                    |

A C or C++ source file path in `@[link(...)]` works in compiler mode — the
compiler compiles it as part of the build. In interpreter mode, source
files cannot be compiled on the fly — they must be pre-compiled to a shared
library and the path updated accordingly.

---

### Memory Model Implementation

The three allocation strategies described in the compiler map to standard
implementation patterns:

**Scope arena** — implemented as a stack of bump-pointer arenas, one per block.
On block entry the frontend emits an `alloca` aggregate (or a bump pointer
advance) sized to hold all locals declared in that block. On block exit it emits
the corresponding deallocation. LLVM's `alloca` instruction is already scope-
lifetime by definition — the frontend's arena framing maps directly onto it with
no additional runtime mechanism needed.

**`#alloc` / `#free`** — implemented as calls to the system allocator (`malloc`/
`free`) with an additional allocation registry (a hash map of live addresses)
that the compiler maintains to catch double-free and null-free. The
registry check is a single hash lookup on every `#free` call.

**`#arena_create` / `#arena_alloc` / `#arena_free`** — implemented as a simple
bump-pointer allocator over a `malloc`-ed region. `#arena_alloc` advances a
pointer; `#arena_free` calls `free` on the original region. No per-slot tracking
needed.

**Nullable / fallible tagged slots** — the tag byte is a Lucid-level abstraction.
The frontend lowers `T?` and `T!` declarations to a struct in IR:

```llvm
; T? lowers to:
%nullable_int = type { i8, i32 }   ; tag byte + value

; T?! lowers to:
%combined_int = type { i8, i32 }   ; tag byte + value (tag encodes 0/1/2)
```

Tag reads and writes are ordinary IR load/store instructions on the first field.
The compiler generates these at every nullable/fallible access point —
no runtime library needed.

---

## Notation (EBNF)

```ebnf
rule        = ...           (* definition *)
A B                         (* sequence *)
A | B                       (* alternative *)
( A )                       (* grouping *)
[ A ]                       (* optional — 0 or 1 *)
{ A }                       (* zero or more *)
A+                          (* one or more *)
'token'                     (* literal terminal *)
UPPER                       (* lexical token defined in Lexical section *)
```

---

## Lexical Tokens

```ebnf
IDENTIFIER  = LETTER { LETTER | DIGIT | '_' }
LETTER      = 'a'..'z' | 'A'..'Z' | '_'
DIGIT       = '0'..'9'

INT_LIT     = DIGIT+
            | '0x' HEX_DIGIT+
            | '0b' BIN_DIGIT+
            | '0o' OCT_DIGIT+

FLOAT_LIT   = DIGIT+ '.' DIGIT+ [ ( 'e' | 'E' ) [ '+' | '-' ] DIGIT+ ]

STRING_LIT  = '"' { STRING_CHAR | interpolation } '"'
            | '"""' { ANY_CHAR_EXCEPT( '"""' ) } '"""'
              (* triple-quote raw string — no escape processing, no
                 interpolation. Content may include single and double
                 quote characters freely; the string only closes at the
                 next occurrence of three consecutive quotes. May span
                 multiple lines. Lexed at higher priority than '"' so
                 three consecutive quotes are always the multiline
                 delimiter, never an empty string followed by a new one.
                 The only sequence a triple-quote raw string cannot
                 contain is '"""' itself — there is no escape mechanism
                 inside one for that. *)

STRING_CHAR = ANY_CHAR_EXCEPT( '"', '\', NEWLINE )
            | '\' ( 'n' | 't' | 'r' | '\' | '"' | '0' )   (* escape sequence *)

interpolation = '\(' expr ')'
                (* embeds an expression directly in a string literal — see
                   String Interpolation, under Type Conversion.
                   Only valid inside "..." — forbidden inside """...""". *)

CHAR_LIT    = '\'' CHAR_CHAR '\''

BOOL_LIT    = 'true' | 'false'

HEX_DIGIT   = DIGIT | 'a'..'f' | 'A'..'F'
BIN_DIGIT   = '0' | '1'
OCT_DIGIT   = '0'..'7'

COMMENT     = '--' { ANY_CHAR } NEWLINE     (* line comment *)
            | '{-' { ANY_CHAR } '-}'        (* block comment, nestable *)
```

---

## Keywords

```
let         const       struct      enum        trait       import
as          if          else        switch      case        default
return      break       continue    while       for         in
do          await       async       spawn       join        and         
or          not         true        false       nil         err
```

> [!NOTE]
> `pub`, `export`, `extern` and `link` are **not** keywords. Visibility and
> linkage are expressed as attributes: `@[export]`, `@[foreign("C")]`,
> `@[link("libname")]`.
>
> `impl`, `from`, `trait`, `type`, `package` do not exist in Lucid. Package
> identity is determined by the file path relative to the package root declared
> in the build manifest — no per-file declaration is required.
>
> **`type` (type alias) was deliberately considered and rejected — see below.**

> [!WARNING]
> **Type alias (`type X = Y`) was deliberately rejected — do not re-propose it
> without solving the problem below first.**
>
> The blocking problem is not weak motivation (a transparent primitive alias
> like `type cm = int` was also considered and found to add no language-enforced
> safety — a good variable name or a doc comment communicates the same thing).
> The blocking problem is **structural**: a type alias that names a function
> type breaks parsing itself, not just readability.
>
> `func_decl` only ever introduces parameter names by writing `(name type)`
> directly in a literal `param_group` — see **Function Declaration**. Consider:
>
> ```lucid
> type MagicFunction = (a int) -> int;
>
> const doSomething MagicFunction = { return a + 5 }
>```
>
> This single line is **structurally ambiguous to programmer**. We
> cannot know `a` is a parameter name introduced by it, without first
> resolving what the alias means. That resolution is a type-checking concern,
> but the decision of *which grammar production this even is* — `var_decl`
> vs. a function declaration — has to happen during parsing, before type
> checking exists. This inverts the normal parse-then-typecheck pipeline and
> was judged not worth the complexity it would force onto the rest of the
> language.
>
> A future proposal must solve this structural problem — not just argue the
> feature is useful — before being reconsidered. One direction that avoids it
> entirely: restrict any alias to types that can never appear bare on the
> left of `const IDENTIFIER <alias> = expr` in a way that could be confused
> with a function declaration (e.g. forbid aliasing function types
> specifically, while still allowing struct/enum/generic aliases — those
> don't introduce named parameters and don't share `func_decl`'s ambiguous
> surface form). This was not pursued further because, combined with the
> primitive case already being weak, the remaining justification for the
> feature was judged too narrow to be worth the added grammar complexity.

---

## Comments

```ebnf
line_comment    = '--' { ANY_CHAR } NEWLINE

block_comment   = '/-' { ANY_CHAR | block_comment } '-/'   (* nestable *)

doc_comment     = '/--' { ' -' ANY_CHAR NEWLINE } '--/'
```

```lucid
-- this is a line comment

/-
    this is a block comment
    /- it can be nested -/
-/

/    --
 - document comment — attached to the immediately following declaration
 - each continuation line starts with ' -'
 - Markdown content is supported
--/
```

### Lexer Disambiguation

`/-` and `/--` both start with `/`:

- `/` followed by `--` → doc comment `/--`
- `/` followed by `-` → block comment `/-`
- `/` alone → division operator

### Doc Comment Attachment Rules

1. Stacked `--` lines immediately above → attach as stacked doc
2. `--` on the same line → trailing doc
3. `/-- --/` immediately above → block doc
4. Stacked above + trailing on same line → stacked wins, trailing ignored
5. Blank line between comment and declaration → comment is floating, not attached
6. `--` lines above a `/-- --/` block → block attaches; `--` lines above it are floating

```lucid
-- normalizes the vector in place
-- only call after the vector has been validated
const normalize (v Vec2) -> Vec2 = { ... }    -- stacked attaches

const maxVertices int = 65536;    -- Vulkan hard limit   -- trailing attaches

/    --
 - Computes the dot product of two vectors.
 -
 - Returns `|a| * |b| * cos(angle)`.;
--/
const dot (other Vec2) -> float = { ... }    -- block attaches
```

---

## Separators `,` and `;`

Commas `,` and semicolons `;` are **optional** — Lucid uses newlines and block
structure to delimit constructs. The parser accepts them but never requires them.

---

## Top-Level Structure

```ebnf
program         = { top_level_item }

top_level_item  = { attribute_list } top_level_decl
                | import_decl

top_level_decl  = struct_decl
                | enum_decl
                | func_decl
                | var_decl
```

---

## Module System

Every `.lucid` file is a module. A module's identity is its file path relative
to the package root. The package name and root are declared in the build manifest
(e.g. `lucid.toml`) — no `package` declaration is needed in source files.
Modules are flat — nesting is not supported.

```ebnf
import_decl    = 'import' use_path [ 'as' IDENTIFIER ]

use_path    = IDENTIFIER { '.' IDENTIFIER }
```

```lucid
import std.io
import std.math as math
import graphics.gl as gl
```

### Exported Name Immutability

`@[export]` makes a name visible outside its module, but the binding belongs to
the module that declared it. From outside the module, exported names are always
**read-only** — they cannot be reassigned regardless of whether the declaration
is `let` or `const` internally:

```lucid
-- inside mymod.lucid
@[export] let counter int = 0;    -- mutable inside the module
@[export] const PI float = 3.14;    -- immutable everywhere

-- inside another module
import mymod

mymod:counter = 1;    -- ERROR: exported names are read-only from outside the module
mymod:PI      = 3.0;    -- ERROR: same rule
```

This applies to all exported declarations — variables, functions, structs, enums,
and traits. A module's exported surface is its public API; external code can read
and call, never reassign.

### Module Member Access

Module members are accessed with `:`, not `.`. The colon is a deliberate
syntactic distinction — it signals that the left-hand side is a module, not a
struct, and that the result is **always read-only**. You can never assign through
`:`.

```ebnf
module_expr = IDENTIFIER ':' IDENTIFIER          (* module:member *)
            | IDENTIFIER ':' call_expr            (* module:func(...) *)
            | IDENTIFIER ':' generic_expr          (* module:genericFunc<T>(...) *)
```

```lucid
import std.math as math
import std.io   as io

-- reading an exported value
let tau float = math:TAU;

-- calling an exported function
let result float = math:sqrt(2.0);

-- piping through an exported function
let normalized float = 3.14 |> math:clamp;

-- chaining: result of a call is a value, access its fields with .
let len int = math:split("a,b,c"):length;    -- ERROR: : is only for module access
let parts [*]string = math:split("a,b,c");
let len   int       = parts.length;    -- OK: . for struct field
```

**`:` vs `.` — the rule:**

| Syntax        | Left-hand side | Assignable | Example         |
| ------------- | -------------- | ---------- | --------------- |
| `module:mem`  | module name    | never      | `math:sqrt(x)`  |
| `value.field` | struct value   | if `let`   | `player.health` |

The compiler rejects `module:member = ...` at the assignment site regardless
of the member's internal mutability. This is enforced syntactically —
`:` never produces an l-value.

#### Depth of the Read-Only Guarantee

"Always read-only" applies to **everything reachable through `:`**, not just
the immediate result — a struct obtained via `:` is treated as `const` at
every field depth, regardless of how its fields are individually qualified
internally. This distinguishes two cases that look similar but are not:

```lucid
-- the module exports a FUNCTION that returns a new struct each call
@[export] const makeUser () -> User = { return User { id = 0  name = ""  email = "" } }

const u User = mymod:makeUser();
u.name = "alice";    -- OK: u is a fresh value the caller owns outright
                            -- mymod:makeUser() is the module_expr, already
                            -- fully resolved to a plain User before '.name'
                            -- is ever reached

-- the module exports a STRUCT VALUE directly (a package-level variable)
@[export] let currentUser User = User { id = 1  name = "bob"  email = "" }

mymod:currentUser.name = "eve";    -- ERROR: cannot assign through a value
                                    -- obtained via ':', at any field depth
```

The difference is **what produced the value**, not its shape. A function
call through `:` returns a brand-new value the module has no further claim
over — ordinary `.field` mutation rules apply from that point on, exactly as
they would for a value from any other source. A struct reached by naming an
exported binding directly is still, transitively, *the module's own storage*
— allowing `.field` to reach through and mutate it would silently defeat
**Exported Name Immutability** for every exported struct, just by routing
the same mutation through one extra `.field` hop instead of a direct
assignment.

#### Pattern: Controlled Mutation Without Methods

Lucid has no methods, no `self`, and no per-field accessor syntax (see the
opening note on removed features). The equivalent of a getter/setter is an
ordinary exported function — nothing new to learn, and consistent with how
every other module export already works. Keep the mutable state itself
**unexported**, and export plain functions that read or write it under the
module's own control:

```lucid
-- inside config.lucid
struct Config {
    threshold int
}

let current Config = Config { threshold = 10 }    -- NOT exported

@[export] const getThreshold () -> int = { return current.threshold }

@[export] const setThreshold (v int) -> () = {
    if v < 0 { return }    -- the module can validate, log, or guard here
    current.threshold = v;
}
```

```lucid
-- from outside the module
import config

const t int = config:getThreshold();
config:setThreshold(20);
config:current.threshold = 5;    -- ERROR: current is not exported at all,
                                  -- and even if it were, '.' cannot reach
                                  -- through ':' to mutate it
```

This gives the module exactly the same control a getter/setter would —
validation, logging, invariants on write — without introducing instance
methods, `self`, or accessor syntax that would conflict with the rest of the
grammar.

#### Why This Is More Than a Getter/Setter Substitute

A class-based getter/setter is bound to **one object** — every instance owns
its own copy of the data and its own pair of accessor methods. A module
managing its own state has no such constraint: there is exactly one module,
and it is free to store the data underneath its exported functions in
whatever layout actually performs best, completely independently of how
callers ask for it.

**The data does not have to be "one struct per logical item."** A module can
hold many logical `User`s as one **array of structs** (`[*]User` — natural,
direct, mirrors how the data is used) or restructure the same data as a
**struct of arrays** — one `[*]int` for every `id`, one `[*]string` for every
`name`, and so on — purely for cache locality, with no change visible to
callers. This is a real, common performance technique: iterating a single
`[*]int` of `id`s touches only `id` data, with no unrelated `name`/`email`
bytes sharing the same cache lines the way they would inside `[*]User`. See
**Arrays** for the array shapes this can be built from. Whether to take this
step is a judgment call:

- **Array of structs** — the data is usually accessed and updated **as whole
  records** (read a `User`, write a `User`). Reach for this first; it is
  simpler and matches how the data is naturally used.
- **Struct of arrays** — the data is iterated over **one field at a time, at
  scale** (sum every `score`, find the minimum `distance` across thousands of
  entries) and that access pattern dominates. Reach for this only once
  profiling shows the field-at-a-time access is actually a bottleneck — it is
  a real restructuring of the data, not a free upgrade, and it costs
  readability the array-of-structs form does not.

Either way, **none of this is visible outside the module.** The exported
functions are the only contract callers depend on — the module can change its
internal layout later, in either direction, without breaking a single caller.
This is the actual advantage over a traditional class: a class's public shape
*is* its field layout (or is coupled to it through generated accessors), so
changing how a class stores its data is a breaking change for anyone touching
its fields directly. A module's exported functions are already a layer of
indirection over the storage, for free.

**Overloading lets the exported surface offer multiple views without
multiplying the underlying storage.** Since Lucid resolves overloads by
parameter shape (see **Function Overloading**), one module can expose several
ways to retrieve the same underlying data, each shaped for a different
caller need:

```lucid
-- inside users.lucid — internal layout is the module's own choice
let ids    [*]int    = [];
let names  [*]string = [];

@[export] const getUser (id int)    -> User?    = { ... }    -- one full record
@[export] const getUser (ids [*]int) -> [*]User  = { ... }    -- many full records
@[export] const getUser ()           -> [*]int   = { return ids }    -- ids only, no copy of names
```

```lucid
-- from outside the module — caller picks the shape it actually needs
const one  User?  = users:getUser(7);
const many [*]User = users:getUser([7, 8, 9]);
const all  [*]int  = users:getUser();
```

Each overload can be implemented against whatever internal layout is
fastest for that access pattern — the caller only ever sees `getUser`, never
the storage decision behind it.

---

## Primitive Types

```ebnf
primitive_type  = int_type | float_type | 'bool' | 'byte'
                | 'string' | 'char' | 'nil'
                (* 'any' is intentionally absent — all types are explicit in Lucid.
                   Raw foreign memory uses *uint8 instead. *)

int_type        = 'int8'  | 'int16'  | 'int32'  | 'int64'
                | 'uint8' | 'uint16' | 'uint32' | 'uint64'
                | 'int'   | 'uint'
                | 'byte'  | 'short'  | 'long'
                | 'ubyte' | 'ushort' | 'ulong'

float_type      = 'float' | 'double' | 'decimal'
```

---

## Type System

```ebnf
type            = primitive_type
                | struct_type
                | enum_type
                | func_type
                | array_type
                | ptr_type
                | generic_type
                | nullable_type
                | fallible_type
                | IDENTIFIER                (* named type reference *)

nullable_type   = type '?'
fallible_type   = type '!'

array_type      = '[' array_size ']' type
array_size      = '*'       (* owned heap array — Lucid owns the memory *)
                | '_'       (* slice — borrowed view, Lucid does not own *)
                | INT_LIT   (* fixed-size stack array *)

ptr_type        = '*' type                  (* raw pointer — FFI use only *)
                | '*' func_type             (* pointer to a C function — callback parameters *)

generic_type    = IDENTIFIER '<' type_arg { ',' type_arg } '>'
type_arg        = type | INT_LIT
```

---

## Struct Declaration

A struct is a named holder — a collection of typed fields, each on its own
line. A struct may implement one or more traits by listing them after `:`.

```ebnf
struct_decl     = 'struct' IDENTIFIER [ generic_params ]
                  [ ':' trait_ref { ',' trait_ref } ]
                  '{' { struct_field } '}'

struct_field    = { attribute_list } IDENTIFIER type [ '=' expr ]
                  (* mutable field by default — same as let *)
                | { attribute_list } IDENTIFIER 'const' type [ '=' expr ]
                  (* const field — cannot be reassigned after construction.
                     name then type, optional default value. One rule governs
                     both: a const field's value must exist by the end of the
                     struct literal. If a default is declared, the literal may
                     omit it (falls back to the default) or override it (the
                     explicit value wins). If no default is declared, the
                     literal must supply a value — there is nothing to fall
                     back to. Either way, whatever value the field holds when
                     construction finishes is fixed for the lifetime of the
                     value — see Const Fields, below. *)

trait_ref       = IDENTIFIER
                | IDENTIFIER '<' type_arg { ',' type_arg } '>'

generic_params  = '<' generic_param { ',' generic_param } '>'
generic_param   = IDENTIFIER
                | IDENTIFIER ':' trait_ref { ',' trait_ref }
                  (* constrained generic — T must implement listed traits *)
```

```lucid
struct Point {
    x float = 0.0
    y float = 0.0
}

struct Node<T> {
    value T
    next  *Node<T>?    -- nullable pointer to next node
}

struct Player {
    name   string
    health int    = 100
    speed  float  = 1.0
    active bool   = true
}

-- struct implementing traits
struct Entity : Vector2, Named {
    name   string    -- satisfies Named
    x      float = 0.0    -- satisfies Vector2
    y      float = 0.0    -- satisfies Vector2
    health int   = 100
}

-- field typed as a trait — accepts any struct implementing Vector2
struct PhysicsBody {
    position  Vector2    -- any struct implementing Vector2
    velocity  Vector2
    mass      float = 1.0
}

-- generic struct with trait constraint
struct Wrapper<T : Named> {
    item  T
    label string
}

-- deprecated field — built-in attribute
struct Config {
    @[deprecated("use maxConnections instead")]
    max_conn       int    = 100
    maxConnections int    = 100
    host           string = "localhost"
}
```

### Struct Initialization

Structs are initialized with a struct literal — field names followed by `=`
and a value. Fields with default values may be omitted:

```lucid
-- all fields explicit
const p Point = Point { x = 3.0, y = 4.0 }

-- omit fields that have defaults
const origin Point = Point {}    -- x=3.0, y=4.0 from defaults
const shifted Point = Point { x = 5.0 }    -- x=5.0, y=4.0 from default
```

> [!NOTE]
> This rule applies the same way to `const` fields — a default declared on a
> `const` field is a fallback, not a fixed value baked into the type.
> Omitting it at the literal takes the default; supplying it overrides the
> default. Either way, whatever the field holds once the literal finishes
> evaluating is what it holds for the value's entire lifetime — `const` only
> ever governs what happens *after* construction, never *how* construction
> fills the field in. A `const` field with **no** declared default has no
> fallback to take, so the literal must supply one:
>
> ```lucid
> struct Counter {
>     const step int = 1;    -- has a default — literal may omit or override
>     total      int
> }
>
> let a Counter = Counter { total = 0 }              -- step = 1 (default)
> let b Counter = Counter { step = 5, total = 0 }    -- step = 5 (override)
> a.step = 2;    -- ERROR: step is const — fixed once construction finished
>
> struct Validator {
>     const check (int) -> bool;    -- no default — literal MUST supply one
> }
>
> const v Validator = Validator { }    -- ERROR: check has no default and
>                                             -- was not supplied
> const v2 Validator = Validator {
>     check = (n int) -> bool { return n > 0 }    -- OK: required, now fixed
> }
> ```

```lucid
-- nested struct
struct Rect {
    origin Point
    width  float = 0.0
    height float = 0.0
}

const r Rect = Rect {
    origin = Point { x = 1.0, y = 2.0 }
    width  = 100.0;
    height = 50.0;
}

-- generic struct
struct Box<T> { value T }

const b1 Box<int>    = Box<int>    { value = 42 }
const b2 Box<string> = Box<string> { value = "hello" }
const b3 Box<Point>  = Box<Point>  { value = Point { x = 1.0, y = 2.0 } }

-- mutable struct — fields can be reassigned
let player Player = Player { name = "hero" }
player.health = 80;
player.speed  = 1.5;

-- struct with nullable field
struct Enemy {
    name   string
    target Player?    -- nullable — may have no target
}

const e Enemy = Enemy { name = "goblin", target = nil }
const e2 Enemy = Enemy { name = "orc", target = player }
```

### Field Access

Fields are accessed with `.`:

```lucid
const px float = p.x;    -- 3.0
const py float = p.y;    -- 4.0
const rw float = r.width;    -- 100.0
const ox float = r.origin.x;    -- 1.0  (nested access)

-- nullable field — guard before access
if e2.target != nil {
    const hp int = e2.target.health;
}
```

### Const Fields and Function-Typed Fields

A field declared with `const` cannot be reassigned through `field_expr`, even
when the containing variable is itself `let`. Field-level `const` is
part of the struct's own definition, not something the holder of a mutable
variable can override — `player Player` makes `player`'s *mutable*
fields reassignable through `player.field = ...`, but a field the struct
itself declared `const` stays read-only regardless:

```lucid
struct Counter {
    const step int = 1;    -- has a default — see Struct Initialization
    total      int
}

let c Counter = Counter { total = 0 }    -- step = 1, taken from the default
c.total = 5;    -- OK: total is let
c.step  = 2;    -- ERROR: step is const — read-only even though c is let
```

Whether a `const` field's fixed value came from a declared default or an
explicit value at the literal makes no difference here — `const` only
governs what happens *after* construction (see **Struct Initialization** for
the default/override/required rule that governs construction itself).

> [!NOTE]
> **When to reach for a `const` field.** If a value is genuinely the same
> for every instance of a struct, a per-instance `const` field stores a
> redundant copy in every value — prefer a module-level `const` or an `enum`
> variant instead, and reference it from a default if one is still useful
> (`const step int = Defaults.STEP;`). A `const` field earns its per-instance
> storage when the value is expected to legitimately differ between
> instances — which is most often true of behavior, not plain data. This is
> why `const` fields of **function type** (below) are the most common
> legitimate use: each instance can be constructed with different, fixed
> behavior, and a function value is a single pointer-sized slot regardless
> of how complex that behavior is, so there is no duplication cost the way
> there is for a repeated scalar.

This applies the same way to a field of **function type**. Luc introduced
`impl` partly to prevent a struct's behavior from being reassigned after
construction — Lucid has no `impl` and no methods at all (see the opening
note on removed features), so this concern only ever applies to an ordinary
field that happens to hold a function value, and `const` already covers it
with no further mechanism needed. A behavior field like this is also the
clearest case for a `const` field with **no** default — every instance is
expected to supply its own behavior, so there is no sensible fallback to
declare, and the literal is required to provide one (see **Struct
Initialization**):

```lucid
struct Validator {
    const check (int) -> bool;    -- no default — every instance supplies its
                                          -- own behavior; fixed once construction
                                          -- finishes
}

const positive Validator = Validator {
    check = (n int) -> bool { return n > 0 }
}

positive.check = (n int) -> bool { return n < 0 }    -- ERROR: check is const

const result bool = positive.check(5);    -- OK: calling through a
                                                  -- const field is unaffected
                                                  -- only reassignment is blocked
```


A field left `let` (the default, same as any other declaration) can be
reassigned freely, including to a different function value — useful for
genuinely swappable behavior, like a configurable callback:

```lucid
struct Logger {
    sink (string) -> () = (msg string) -> () { io:printl(msg) }
}

let log Logger = Logger { }
log.sink = (msg string) -> () { system:writeToFile("app.log", msg) }    -- OK
```

### Security Considerations for Function-Typed Fields

A function-typed field is the closest thing Lucid has to an injectable
dependency — construction can supply different behavior per instance, similar
in spirit to an interface. The compiler only guarantees the **shape** of what
gets supplied (the exact `func_type`, checked at every assignment); it
guarantees nothing about what that function actually *does* when called — a
correctly-shaped function can still perform arbitrary I/O, foreign calls, or
side effects. There is no capability or whitelist mechanism in the language
that restricts *which* functions may be supplied beyond matching the
declared shape, so the following are the practical mitigations available:

- **Use `const`, not `let`, for any function field where "was this changed
  later" is a security question, not just a convenience one** (auth checks,
  validators, permission callbacks). A `let` function field — the `Logger`
  pattern above — is reassignable for the entire lifetime of the value by
  anyone holding a mutable reference to it, not just whoever constructed it;
  a function taking `&T` can rewrite it through the reference, silently
  changing behavior for the original owner too (see **Borrowed Types —
  Scoped References**). `const` closes that window entirely: whatever was
  supplied at construction is fixed for the value's lifetime, as shown by
  `Validator.check` above. `const` only closes the *reassignment* window —
  it does not vet the function supplied at construction; that trust decision
  still has to happen at the call site that builds the value.

- **Prefer a closed `enum` dispatched with `switch` over an open function
  parameter whenever the set of valid behaviors is small and known ahead of
  time.** `switch` on an enum is exhaustiveness-checked by the compiler (see
  **`switch`** — **Enum exhaustiveness**): every variant must be explicitly
  handled or the build fails. This gives a closed, auditable, compiler-
  verified set of possible behaviors, which a function-typed parameter
  fundamentally cannot — the space of "any function with this shape" is
  unbounded, the space of enum variants is not.

  ```lucid
  -- avoid: caller supplies arbitrary behavior matching the shape
  const runCallback (call () -> ()) -> () = { call() }

  -- prefer: caller selects from a closed, exhaustively-checked set;
  -- the actual behavior is never exposed as a parameter at all
  enum Action { Save = 0  Reload = 1  Discard = 2 }

  const constructAndRunAction (kind Action)(arg1 T)(arg2 U) -> () = {
      switch kind {
          case Action.Save:    { doSave(arg1, arg2) }
          case Action.Reload:  { doReload(arg1) }
          case Action.Discard: { doDiscard() }
      }
  }
  ```

  Keep the concrete handlers (`doSave`, `doReload`, `doDiscard`) un-exported
  so they are unreachable by name from outside the module — the dispatcher
  is then the only entry point, and the compiler proves no case was missed.

- **Reserve genuinely open, caller-supplied behavior for cases where it is
  the actual point of the API** (plugins, strategy objects). There, the
  mitigation is not prevention — the API needs the injection point — but
  bounding the window (`const`) and controlling who is trusted to construct
  the value in the first place.

---

## Trait Declaration

A trait is a pure **field contract** — a named set of fields (name and type
only) that a struct promises to contain. Traits have no methods, no behavior,
and no default values. They exist solely to express structural requirements 
for data polymorphism.

```ebnf
trait_decl  = 'trait' IDENTIFIER [ generic_params ] '{' { trait_field } '}'

trait_field    = { attribute_list } IDENTIFIER type
                | { attribute_list } 'const' IDENTIFIER type
```

```lucid
trait Vector2 {
    x float
    y float
}

trait Named {
    name string
}

trait Bounded {
    minVal float
    maxVal float
}

-- generic trait
trait Container<T> {
    value T
    count int
}
```

### Rules

- A struct implementing a trait must declare all the trait's fields at the
  **top level** with matching names and types.
- Field type mismatch is an **error** at the struct declaration site.
- Two traits requiring the same field name with **different types** is a
  **compile error** at the struct declaration — there is no silent merging.
  Same name, same type across two traits is fine — satisfied once.
- Traits may be used as **field types** in structs — a field typed as a trait
  accepts any struct implementing that trait.
- Traits may be used as **parameter types** in functions — inside the function
  body only the trait's fields are accessible on that parameter.
- Traits may be used as **generic constraints** — `<T : Trait>` means T must
  implement Trait. See **Generic Constraints**.

```lucid
-- name conflict: same field, different types — compile error
trait A { x float }
trait B { x int   }

struct Bad : A, B {    -- ERROR: field x required as float by A and int by B
    x float    -- which one?
}

-- name conflict: same field, same type — fine, satisfied once
trait A { x float }
trait B { x float, y float }

struct Both : A, B {    -- OK: x satisfies both A and B
    x float
    y float
}
```

---

## Enum Declaration

An enum is a set of named variants, each with a **required** integer value —
Lucid does not auto-increment omitted values, the same no-inference stance
applied everywhere else in this grammar (see **Variable Declaration**: a
type is always written, even where the answer looks unambiguous; the same
principle applies here to values, not just types). Visually: a vertical
block where each variant is a row with a name cell and a value cell.

```ebnf
enum_decl       = { attribute_list } 'enum' IDENTIFIER [ ':' int_type ] 
                  '{' { enum_variant } '}'

enum_variant    = { attribute_list } IDENTIFIER '=' INT_LIT
                  (* value is required — no auto-increment. Omitting it and
                     letting the compiler infer previous + 1 would be the
                     same category of guess this grammar rejects for var_decl
                     and for-loop types; an enum is no exception. *)
```

```lucid
enum Direction {
    North = 0
    East  = 1
    South = 2
    West  = 3
}

enum Status : int32 {
    Ok      = 200
    NotFound = 404
    Error   = 500
}

enum Bad {
    North    -- ERROR: value is required, even though "the next int" looks obvious
    East  = 1
}
```

---

## Function Declaration

A function declaration binds a name to a function value. Visually: a horizontal
block where each parameter group is a column, separated by `->` arrows that mark
real execution boundaries.

```ebnf
func_decl       = { attribute_list } ('let' | 'const') IDENTIFIER [ generic_params ]
                  param_group
                  [ '->' return_type ]  (* omitted for void/unit *)
                  '=' func_body

param_group     = '(' [ param_list ] ')'
param_list      = param { ',' param } [ ',' variadic_param ]
                | variadic_param
param           = IDENTIFIER type               (* pass by value — caller's copy *)
                | 'const' IDENTIFIER type       (* read-only reference parameter *)
variadic_param  = IDENTIFIER '...' type
                  (* must be the last parameter of a param_group; collects zero or
                  more trailing arguments into a [*]type array *)

return_type     = type
                | '(' type ',' type { ',' type } ')'   (* multiple return values *)

func_body       = '{' { statement } '}'         (* declaration only — see WARNING below;
                                                     invalid as the RHS of a reassignment *)
                | expr                           (* a function body may be ANY expr that
                                                     evaluates to a function value: a single
                                                     computed result, an anonymous func_literal,
                                                     a named reference, a generic instantiation,
                                                     or a call that returns a function — see
                                                     Function Body as an Expression *)

generic_args    = '<' type_arg { ',' type_arg } '>'
                  (* instantiates a generic function as a VALUE — no call parens.
                     Distinct from generic_expr, which calls the function:
                     'Foo<int>' is a reference (an IDENTIFIER-headed expr);
                     'Foo<int>(42)' is a call_expr. Both are 'expr'. *)

func_type       = param_group '->' return_type
                  (* a function type is always a single param group *)
```

**`func_body`'s second form is just `expr` — nothing narrower.** Earlier drafts
of this grammar introduced a separate `func_ref` production to describe "an
anonymous function or a named reference," but that was a hand-rolled subset of
something `expr` already covers: `IDENTIFIER`, `call_expr`, `module_expr`, and
`generic_expr` are already alternatives of `expr` (see **Expressions**). A
`func_decl` whose body is not a literal block is therefore bound to whatever
`expr` evaluates to, as long as the result's type matches the declared
`func_type`. This means the body can be:

```lucid
-- a func_literal (anonymous function value)
let f () -> () = () -> () { ... }
f = () -> () { ... }

-- a named function reference — no call, IDENTIFIER alone
const sq<T> (v T) -> T = { return v * v }
let g (a int) -> int = sq<int>
g = myModule:sq<int>          -- module-qualified, still just an expr

-- a call_expr that itself RETURNS a function value
const getHandler (kind string) -> (int) -> int = { ... }
let h (a int) -> int = getHandler("double")
h = getHandler("triple")      -- reassignment from a call is equally valid
```

The last case is the one easy to miss: the right-hand side does not have to be
the function value written out directly — it can be any expression, including a
call, as long as evaluating it produces a value of the matching `func_type`.
`getHandler("double")` is a `call_expr`; `call_expr` is an `expr`; `expr` is what
`func_body`'s second form accepts. No special-casing needed beyond that.

Reassignment (`f = ...`) follows ordinary `assign_stmt` rules — `f` must be
`let`, and the right-hand side `expr` must evaluate to a value whose type
matches `f`'s declared `func_type` exactly.

> [!WARNING]
> **A block body (`{ ... }`) is only valid at the declaration, never at
> reassignment.** A bare block carries no parameter list, no parameter types,
> and no return type of its own — at declaration time it borrows all of that
> from the surrounding `func_decl` header (`param_group`, `return_type`). At
> reassignment there is no such header to borrow from: `f = { ... }` would
> leave both the parser and the reader unable to tell what parameters `f`
> takes inside that block. Reassignment must therefore use an `expr` that
> already carries or produces its own signature — a `func_literal`, a named or
> module-qualified reference, or a call returning a function value — never a
> bare block.
>
> ```lucid
> -- declaration: bare block is valid — header supplies the signature
> let f (a int) -> int = { return a + 1 }
>
> -- reassignment: bare block is REJECTED — no header to borrow from
> f = { return a + 2 }    -- ERROR: block body not allowed outside declaration
>
> -- reassignment: anonymous function — OK, carries its own signature
> f = (a int) -> int { return a + 2 }
>
> -- reassignment: named reference — OK, signature comes from the reference
> const addTwo (a int) -> int = { return a + 2 }
> f = addTwo
>
> -- reassignment: call returning a function — OK, signature comes from the
> -- call's return type
> const pickAdder (n int) -> (int) -> int = { ... }
> f = pickAdder(2)
> ```

**Multiple return values** are grouped by `()`, NOTE: this is not a tuple but a way
to allow parser to differentiate them with a `param_group`

```lucid
const parseInt (s string) -> (int, bool) = {
    -- returns parsed value and whether parsing succeeded
    return 0, false;
}

-- at the call site
let value int;
let ok bool;
value, ok = parseInt("42");

-- or in a single declaration
let value int, ok bool = parseInt("42");
```

> [!NOTE]
> (T, U) in a return type is a return list, not a tuple. Lucid has no tuple type
>  and does not plan to add one — multiple return values are a calling convention, 
> not a first-class value. You cannot store, pass, or nest a return list as a value. 
> If you need to group values, define a struct.


**Parameters** are passed by value (a copy) by default. A `const` parameter
marks a read-only reference — the function sees the caller's original value
but cannot modify it:

```lucid
const sum (nums ...int) -> int = {
    let total int = 0;
    for _, n int in nums { total = total + n }
    return total;
}

const describe (const v Vector2) -> string = {
    -- v is a read-only reference — no copy overhead
    return "(" + stringFromFloat(v.x) + ", " + stringFromFloat(v.y) + ")";
}
```

### Variadic Parameters

A parameter declared with `...` accepts zero or more trailing arguments,
collected into a `[*]type` array inside the function body. A variadic
parameter must be the **last parameter in its own param group** — no
parameters may follow it within that group. This applies independently to
every group when a function is curried into multiple argument groups (see
**Currying and Partial Application**, later in this document): a variadic may
appear in any group, including a non-final one, as long as it is the last
parameter *of that group*.

```lucid
const sum (nums ...int) -> int = {
    let total int = 0;
    for _, n int in nums {
        total = total + n;
    }
    return total;
}

sum();    -- 0
sum(1, 2, 3);    -- 6

-- variadic combined with regular parameters: variadic must come last
const logf (level int, fmt string, args ...string) -> () = {
    -- args is [*]string
}

-- INVALID — variadic is not the last parameter
const bad (nums ...int, label string) -> int = { ... }    -- ERROR

-- INVALID — variadic is not the last parameter of ITS OWN group
const bad2 (nums ...int, label string)(words ...string) -> int = { ... }    -- ERROR
```

A flat `param_list` allows at most one variadic parameter — it must be the
last parameter, so a second one could never follow it within the same group.
A curry function lifts this limit: since each link in the curry chain is its
own function with its own parameter list, each can independently end in a
variadic, giving a function more than one variadic parameter overall, as
long as each is the last parameter of its own group. The `()()` shorthand
(see **Form 2 — `()()` Shorthand**, later in this document) only changes how
that chain is written — it expands to exactly the nested single-argument
functions described in **Form 1**, so the capability comes from currying
itself, not from the shorthand syntax:

```lucid
-- two independent variadics — impossible as a single flat param_list,
-- straightforward as two curried groups
const summarize (nums ...int)(words ...string) -> string = {
    let total int = 0;
    for _, n int in nums { total = total + n }

    let joined string = "";
    for _, w string in words { joined = joined + w + " " }

    return stringFromInt(total) + ": " + joined;
}

summarize(1, 2, 3)("a", "b");    -- nums = [1, 2, 3], words = ["a", "b"]
```

This is one concrete reason to curry a function's parameters at all, beyond
deferring computation between argument groups (see **Form 1 — Explicit
Intermediate `->` Return**, later in this document): it is the only way to
give a function more than one variadic parameter, since a single flat
parameter list permits just one. Writing the curry chain with the `()()`
shorthand instead of nested Form 1 functions is purely a matter of style —
either spelling has the same capability, because one expands into the other.

### Form 1 — Explicit Intermediate `->` Return

Each `->` in the signature corresponds to a real body boundary. The programmer
writes every intermediate `return (...) -> T { ... }` explicitly. Use this form
when code needs to run *between* argument groups — at the point of partial
application, before the inner function is created.

```lucid
const makeAdder (base int) -> (n int) -> int = {
    const adjusted int = base * 2;    -- runs at makeAdder(base)
    return (n int) -> int { return adjusted + n }
}

const addTen (n int) -> int = makeAdder(5);
addTen(3);    -- 13
```

### Form 2 — `()()` Shorthand

Think of Form 2 as a multi-parameter function that the compiler automatically
makes curriable. You write the function exactly as if all the arguments arrive
at once — the body is flat, all parameters are in scope, the logic reads left
to right. The only difference from a plain multi-param function is that `()()`
groups instead of `(,)` commas, which tells the compiler to make each group
independently callable.

The compiler expands Form 2 recursively and exhaustively into Form 1
before semantic analysis. The written body is never modified — only wrapper
layers are generated around it.

```lucid
-- as written
const add (a int)(b int) -> int = {
    return a + b;
}

-- the compiler expands to
const add (a int) -> (b int) -> int = {
    return (b int) -> int {
        return a + b;
    }
}
```

### Currying and Partial Application

Form 2 (`()()`) is pure textual shorthand for Form 1. The compiler
expands it recursively before semantic analysis. The body is never modified.

The core idea:
- `(a T, b U)` — both values arrive in one call
- `(a T)(b U)` — same values, arriving in two separate calls
- `-> T` — the body must return a value of type `T`
- Every `->` in a signature is a promise that something runs at that boundary

```lucid
-- Form 1: code runs between groups
const makeCompiler (config Config) -> (data string) -> string = {
    const compiled CompiledConfig = compile(config);    -- runs once at partial application
    return (data string) -> string { return apply(compiled, data) }
}

-- Mixing Forms: Form 2 groups first, Form 1 explicit return after
const process (a int)(b int) -> (c int) -> int = {
    const sum int = a + b;    -- runs when process(a)(b) is called
    return (c int) -> int { return sum + c }
}

-- Form 2: flat body, compiler handles the wrapping
const clamp (lo int)(hi int)(v int) -> int = {
    if v < lo { return lo }
    if v > hi { return hi }
    return v;
}

-- partial application
const clamp0to100 (v int) -> int = clamp(0)(100);
clamp0to100(42);    -- 42
clamp0to100(200);    -- 100
```

### Entry Point

```lucid
@[export] const main () -> int = {
    return 0;
}

-- with command-line arguments
-- [_]string: slice — the runtime owns the argument buffer, main gets a
-- read-only view. [*]string would be wrong here: that implies main owns a
-- heap copy of all arguments, which the runtime never hands over.
@[export] const main (args [_]string) -> int = {
    return 0;
}
```

---

## Function Overloading

Two or more declaration with the same name but different parameter signatures are
overloads. The compiler picks the correct overload at the call site based on
argument types. Overloads that differ only in return type are a compile error —
the compiler cannot resolve them from the call site alone.

```ebnf
(* overloading is not a grammar construct — it falls out of the ordinary
   func_decl rule. Two func_decl with the same IDENTIFIER and different
   param_group types are valid and together form an overload set. *)
```

### Rules

- Overloads must differ in **parameter count or parameter types**.
- Differing only in return type is an **error**.
- A generic function and a concrete overload of the same name coexist — the
  concrete overload takes priority when argument types match exactly.
- Cross-module overloads with the same name must be qualified at the call
  site: `mymod:process(42)` rather than `process(42)`.

```lucid
-- concrete overloads — same name, different parameter types
const describe (v int)    -> string = { return "int: "    + stringFromInt(v) }
const describe (v float)  -> string = { return "float: "  + stringFromFloat(v) }
const describe (v bool)   -> string = { return "bool: "   + stringFromBool(v) }
const describe (v string) -> string = { return "string: " + v }

describe(42);    -- resolves to (int) -> string
describe(3.14);    -- resolves to (float) -> string
describe(true);    -- resolves to (bool) -> string
describe("hi");    -- resolves to (string) -> string

-- generic and concrete coexist — concrete wins on exact match
const process<T>  (v T)   -> string = { return "generic" }
const process     (v int) -> string = { return "concrete int" }

process<string>("hi");    -- generic: "generic"
process<int>(42);    -- concrete wins: "concrete int"
process(42);    -- concrete wins: "concrete int"

-- return-type-only difference: compile error
const bad (v int) -> string = { ... }
const bad (v int) -> int    = { ... }
-- ERROR: overloads differ only in return type — unresolvable at call site
```


---

## Variable Declaration

Declare a variable with `let` to make it mutable. Write `const` to make a
binding immutable. A type is **always required** — Lucid does not infer a type 
from an initializer expression, even where the initializer looks unambiguous. 
A bare numeric literal like `0.01` does not by itself say whether it means 
`float`, `double`, or `decimal` — these are distinct types with different 
precision and performance characteristics, and silently picking one would be a guess, 
not a fact derivable from the source. The same principle applies uniformly, 
including to literals that happen to match only one type (a string literal 
could only ever be `string`): the rule is stated once, with no per-case 
exceptions to remember.

```ebnf
var_decl    = 'let'   IDENTIFIER type [ '=' expr ]   (* mutable binding *)
            | 'const' IDENTIFIER type [ '=' expr ]   (* immutable binding *)
```

```lucid
let x int     = 42;    -- mutable
const pi float = 3.14159;    -- immutable
const name string = "lucid";

let bad = "lucid";    -- ERROR: type is required, even when the
                     -- initializer's type looks unambiguous
```

---

## Generic Functions and Generic Structs

Type parameters are declared after the function or struct name in `<...>`.
They are resolved at instantiation sites via explicit type arguments. An
uninstantiated generic is not a callable value — it must be instantiated first.

```ebnf
generic_params  = '<' generic_param { ',' generic_param } '>'
generic_param   = IDENTIFIER
                | IDENTIFIER ':' trait_ref { '+' trait_ref }
                  (* '+' separates multiple constraints on ONE parameter;
                     ',' separates parameters. Using ',' for both would make
                     '<T : A, U>' ambiguous between "T constrained by A and
                     U" and "two parameters, T : A and U" — '+' removes that
                     ambiguity entirely. *)
generic_args    = '<' type { ',' type } '>'
```

### Rules

- Every type parameter must be used at least once in the parameter list or
  return type — an unused `T` is a compile error.
- Type parameters are resolved strictly at the call/instantiation site —
  there is no type inference across call boundaries.
- Generic functions and concrete overloads of the same name coexist — the
  compiler picks the concrete overload first if types match exactly, then
  falls back to the generic.
- A generic function body must be valid for **any** `T` — no `if T is X`
  branching. Type-specific logic is passed in as a callback parameter.

### Generic Constraints

A type parameter can be constrained to require one or more traits using `:`.
Multiple constraints on the **same** parameter are separated by `+`, not
`,` — `,` already separates distinct generic parameters in `generic_params`,
so reusing it for constraints would make `<T : A, U>` ambiguous between "one
parameter `T`, constrained by both `A` and `U`" and "two parameters, `T : A`
and `U`". Inside the function body, the constrained fields declared by the
trait are directly accessible on values of that parameter type:

```ebnf
generic_param   = IDENTIFIER
                | IDENTIFIER ':' trait_ref { '+' trait_ref }
```

```lucid
-- T must implement Vector2 (has x float and y float)
const magnitude<T : Vector2> (v T) -> float = {
    return sqrt(v.x * v.x + v.y * v.y);    -- x and y accessible because T : Vector2
}

-- multiple constraints on the SAME parameter — '+' joins them
const describeEntity<T : Vector2 + Named> (v T) -> string = {
    return v.name + " at (" + stringFromFloat(v.x) + ", " + stringFromFloat(v.y) + ")";
}

-- TWO independently-constrained parameters — T and U need not be the same
-- concrete type, only each independently satisfy Vector2. A plain
-- (a Vector2)(b Vector2) signature (see addVectors, below) would accept the
-- same calls and compute the same result here, since this function only
-- ever reads x/y — but writing <T, U> explicitly documents, at both the
-- declaration and the call site, exactly which two concrete types are
-- involved, which matters for readability and for the visual graph node's
-- own signature display even when the trait alone would be enough to
-- typecheck the body:
const distanceBetween<T : Vector2, U : Vector2> (a T)(b U) -> float = {
    const dx float = a.x - b.x;
    const dy float = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
}

-- works on any struct implementing Vector2
struct Point  : Vector2 { x float = 0.0  y float = 0.0 }
struct Entity : Vector2, Named { name string  x float = 0.0  y float = 0.0 }

magnitude<Point>(Point { x = 3.0, y = 4.0 });    -- OK → 5.0
magnitude<Entity>(Entity { name = "hero", x = 3.0, y = 4.0 });    -- OK → 5.0
magnitude<int>(42);    -- ERROR: int does not implement Vector2

-- T = Point, U = Entity — two DIFFERENT concrete types, each satisfying
-- Vector2 independently; distanceBetween never requires them to match
const p Point  = Point  { x = 0.0, y = 0.0 }
const e Entity = Entity { name = "hero", x = 3.0, y = 4.0 }
distanceBetween<Point, Entity>(p)(e);    -- OK → 5.0

-- constraint in struct generic parameter — same rule, '+' for multiple
-- constraints on one parameter, ',' to separate A from B
struct Pair<A : Named, B : Named> {
    first  A
    second B
}
```

**Return type — trait only:**

A function accepting `T : SomeTrait` cannot reconstruct a full value of `T`
to return — `T`'s complete field set is unknown inside the generic function
body (a constrained `T` only guarantees the trait's fields exist, nothing
else). A generic function constrained this way can read and use the trait's
fields, but it can only ever **build and return the trait type itself** — a
fresh value of the compiler-generated minimal struct for that trait, never a
reconstructed `T`:

```lucid
const addVectors (v1 Vector2)(v2 Vector2) -> Vector2 = {
    return Vector2 { x = v1.x + v2.x, y = v1.y + v2.y }
}

const e Entity = Entity { name = "hero", x = 1.0, y = 2.0 }
const added Vector2 = addVectors(e)(e);    -- returns Vector2, name discarded
```

If a function genuinely needs to produce a modified value of the caller's
own concrete type `T`, take it by reference (`&T`, see **Borrowed Types —
Scoped References**) and modify the constrained fields through it instead of
building a new struct literal. A plain by-value `T` parameter would not work
here — struct assignment is always a full deep copy (see **Owned Types —
Copied on Assignment**), so mutating a by-value `v T` would only ever modify
a copy, never anything visible to the caller. With `&T`, the mutation is
visible through the original, and nothing about `T`'s full field set needs
to be known, since nothing is reconstructed:

```lucid
const scale<T : Vector2> (v &T)(s float) -> () = {
    v.x = v.x * s;
    v.y = v.y * s;
}

let e Entity = Entity { name = "hero", x = 1.0, y = 2.0 }
scale<Entity>(e)(2.0);    -- e.x and e.y scaled in place through the reference,
                         -- e.name untouched
```

### Generic Functions

The legitimate uses of generic functions in Lucid are:

**Opaque pass-through — the function never inspects `T`, only passes it:**

```lucid
const identity<T>  (v T)      -> T      = { return v }
const first<T>     (items [_]T)(length int) -> T? = {
    if length == 0 { return nil }
    return items[0];    -- runtime-checked: a literal index does not prove
                         -- in-bounds against a slice of unknown length
                         -- see Runtime Panics
}
const swap<T>      (a T)(b T) -> (T, T) = { return b, a }
```

**Higher-order — type-specific logic is a callback the caller provides:**

```lucid
const map<T, U>    (items [_]T)(f (T) -> U)           -> [*]U  = {
    let result [*]U = [];
    for _, v T in items { arr:append<U>(result)(f(v)) }
    return result;
}

const filter<T>    (items [_]T)(pred (T) -> bool)     -> [*]T  = {
    let result [*]T = [];
    for _, v T in items { if pred(v) { arr:append<T>(result)(v) } }
    return result;
}

const fold<T, U>   (items [_]T)(seed U)(f (U, T) -> U) -> U   = {
    let acc U = seed;
    for _, v T in items { acc = f(acc, v) }
    return acc;
}

const sort<T>      (items [*]T)(cmp (T, T) -> int)    -> [*]T  = { ... }
```

**Call sites — explicit type arguments always required:**

```lucid
const nums   [*]int    = [3, 1, 4, 1, 5];
const strs   [*]string = ["hello", "world"];

const doubled [*]int    = map<int, int>(nums)((v int) -> int { return v * 2 });
const lengths [*]int    = map<string, int>(strs)((s string) -> int { return strLength(s) });
const evens   [*]int    = filter<int>(nums)((v int) -> bool { return v % 2 == 0 });
const sum     int       = fold<int, int>(nums)(0)((acc int, v int) -> int { return acc + v });
const sorted  [*]int    = sort<int>(nums)((a int, b int) -> int { return a - b });

-- with pipeline
const result [*]string =
    [3, 1, 4, 1, 5, 9, 2, 6]
    |> filter<int>((v int) -> bool { return v > 3 })!;
    |> sort<int>((a int, b int) -> int { return a - b })!;
    |> map<int, string>(stringFromInt)!;
```

### Generic Structs

Structs can carry type parameters too. The type parameter is in scope for all
field type annotations:

```lucid
struct Box<T> {
    value T
}

struct Pair<A, B> {
    first  A
    second B
}

struct Cache<K, V> {
    key   K
    value V
}

-- instantiation
const b Box<int>         = Box<int>    { value = 42 }
const p Pair<int, string> = Pair<int, string> { first = 1, second = "hello" }
```

Functions that operate on generic structs receive the instantiated type:

```lucid
const unbox<T>    (b Box<T>)         -> T = { return b.value }
const rebox<T, U> (b Box<T>)(f (T) -> U) -> Box<U> = {
    return Box<U> { value = f(b.value) }
}

const n int    = unbox<int>(b);
const s Box<string> = rebox<int, string>(b)(stringFromInt);
```

---

## Nullable `?` / Fallible `!`

```ebnf
nullable_type   = type '?'             (* value may be nil *)
fallible_type   = type '!'             (* value may be err *)
combined_type   = type '?!'            (* value may be nil OR err; '?!' is
                                          the only valid order *)
```

```lucid
int?    -- nullable int
string!    -- fallible string
Vector2?!    -- nullable and fallible struct value

[*]int?    -- ERROR: ? on array type is forbidden — use empty array []
             --        instead of a nullable array
(int) -> bool?;    -- ERROR: ? on function type is forbidden
```

> [!NOTE]
> Disallowing nullable/fallible arrays and function types encourages cleaner
> idioms: an empty array `[]` is always preferable to a `nil` array, and an
> empty or no-op function is preferable to a nullable function binding.

### Memory Layout

Every `T?`, `T!`, or `T?!` value occupies a **tagged slot** in the current scope
arena:

```
[ tag: 1 byte | value: sizeof(T) bytes ]
```

The tag encodes the state of the slot:

| Tag | Meaning                                 | Valid on          |
| --- | --------------------------------------- | ----------------- |
| `0` | `nil` — slot is absent, no value        | `T?`, `T?!`       |
| `1` | value present                           | `T?`, `T!`, `T?!` |
| `2` | `err` — slot failed, carries error info | `T!`, `T?!`       |

The compiler allocates the slot when the declaration is entered and
releases it automatically when the scope exits. The user never calls `#alloc` or
`#free` on these — the scope arena manages them.

### Lifetime

Nullable and fallible values live in the **scope arena** of the block that
declares them — not for the entire program. They are released when that block
exits, whether by normal flow, `return`, or `break`.

```lucid
const compute () -> int? = {
    let x int? = 42;    -- x allocated in this scope's arena
    if someCondition {
        let y int? = 10;    -- y allocated in the if-block's arena
        x = y;
    }    -- y released here
    return x;
}    -- x released here
```

### `nil` — Absence

`nil` is a **semantic signal**, not a memory operation. It means "there is no
value here" — a domain concept the programmer expresses intentionally. The
compiler handles the memory automatically regardless; `nil` only
changes the tag.

```lucid
let target Player? = nil;    -- no target selected yet
target = findEnemy();    -- now has a value
target = nil;    -- cleared — not an error, just absent

if target == nil {
    -- no target
} else {
    -- target is Player here (narrowed by the compiler)
}
```

Assigning `nil` sets the tag to `0` and clears the value bytes. The slot remains
in the scope arena and can be reassigned at any time. Memory is reclaimed
automatically when the scope exits — `nil` does not trigger an early free.

### `err` — Failure

A fallible value enters the error state in two ways:

1. **Automatically** — the compiler sets `err` when an operation
   produces a failure, such as division by zero or a failed foreign call:

```lucid
let i int! = 8 / 0;    -- compiler sets tag to 2, stores the error
```

2. **Manually** — the user signals failure explicitly:

```lucid
let result int! = err;    -- explicit failure
let ratio  int! = err;    -- explicit failure
```

`err` is a bare sentinel — it carries no payload and takes no argument. This
keeps `!` simple: it only ever answers "did this fail," never "why." A
function that needs to communicate a reason does so with a separate
out-parameter (see **Error Detail Without a Payload**), not through `err`
itself.

Like `nil`, assigning `err` is a semantic operation. The compiler
manages the memory — the slot is reclaimed when the scope exits.

### Combined `T?!` — Three States

A `T?!` slot can hold three distinct states and the user can move between all of
them explicitly:

```lucid
let x int?! = compute();    -- may arrive as value, nil, or err

x = 42;    -- set to value       (tag 1)
x = nil;    -- set to absent      (tag 0)
x = err;    -- set to failed      (tag 2)

-- Checking all three states
if x == nil {
    -- absent
} else if x == err {
    -- failed — can inspect the error
} else {
    -- has a value
}
```

> [!NOTE]
> `T?!` is the most flexible slot type but also the most demanding to handle
> correctly — all three states must be considered. Prefer `T?` when absence is
> the only concern, and `T!` when failure is the only concern.

---

## Statements

```ebnf
statement       = { attribute_list } decl_keyword decl_stmt ';'
                | assign_stmt ';'
                | if_stmt
                | switch_stmt
                | for_stmt
                | while_stmt
                | do_while_stmt ';'
                | return_stmt ';'
                | break_stmt ';'
                | continue_stmt ';'
                | expr_stmt ';'

block           = '{' { statement } '}'

expr_stmt       = expr
assign_stmt     = expr assign_op expr

return_stmt     = 'return' [ expr ]
break_stmt      = 'break'
continue_stmt   = 'continue'

if_stmt         = 'if' expr block { 'else' 'if' expr block } [ 'else' block ]
                  (* no ';' — block already closes the statement *)

while_stmt      = 'while' expr block
                  (* no ';' — block already closes the statement *)

do_while_stmt   = 'do' block 'while' expr ';'
                  (* ';' required — ends with expr, not a block *)

for_stmt        = 'for' IDENTIFIER type 'in' range_iter [ '..' expr ] block
                  (* Form 1 — range iteration.
                     IDENTIFIER is the loop variable (the current value, not
                     an index — there is no collection). type must be numeric
                     (int, float, etc.). The trailing '..' expr is an optional
                     step; without it the step defaults to 1.
                     REJECTED: a second variable is a semantic error — a
                     step loop carries no index to expose.
                     no ';' — block already closes the statement *)
                | 'for' for_index ',' IDENTIFIER type 'in' expr block
                  (* Form 2 — collection iteration.
                     for_index is the index binding (always int when named);
                     IDENTIFIER is the element value, whose type must match
                     the collection's element type. Both variables require
                     explicit type annotations — Lucid does not infer types
                     from context.
                     REJECTED: a single variable with no index is a semantic
                     error — the index is a required part of the binding,
                     even if it is discarded with '_'.
                     no ';' — block already closes the statement *)

for_index       = IDENTIFIER 'int'    (* named index — always int *)
                | '_'                  (* discard the index entirely *)

range_iter      = expr range_op expr
                  (* start range_op end — reuses range_expr's shape; written
                     as its own production here so the for_stmt form is easy
                     to read directly off this grammar block *)

switch_stmt     = 'switch' expr '{' { case_clause } [ default_clause ] '}'
                  (* no ';' — block already closes the statement.
                     exhaustiveness: if expr is an enum type and no
                     default_clause is present, the compiler errors on
                     missing variants *)

case_clause     = 'case' case_value { ',' case_value } ':' block

default_clause  = 'default' ':' block

case_value      = literal
                | IDENTIFIER '.' IDENTIFIER      (* enum variant *)
                | literal range_op literal       (* literal range — see below *)
                  (* case_value is deliberately narrow — only a bare literal,
                     an enum variant, or a literal range. It is NOT 'expr': a
                     comparison (case a < b), a fallback (case err ?? something),
                     a function call, or any other general expression is
                     rejected. The range form's bounds are themselves literals
                     only, not arbitrary expr — 'case a..b' with variable
                     bounds is rejected for the same reason. switch dispatches
                     on a matched VALUE, never on a computed condition — that
                     is what 'if'/'else if' is for. See Rejected Case Values
                     below. *)
```

> [!NOTE]
> **Semicolon rules at a glance:**
> - Statements that end with a `block` (`if`, `for`, `while`, `switch`) do **not** take a `;` — the closing `}` is unambiguous.
> - `do`/`while` ends with an expression, not a block, so it **does** require `;`.
> - Everything else — declarations, assignments, `return`, `break`, `continue`, bare expression statements — requires `;`.

> [!WARNING]
> **Visibility inside blocks:** `@[export]` is **not allowed** on any local declaration — it is top-level only. The parser emits an error if it appears inside a block.
>
> **Attributes on local declarations:** Attributes (`@[inline]`, `@[deprecated]`, etc.) **are allowed** on local declarations (`struct`, `enum`, `func`, `var`). They are attached to the declaration node and may be used by the semantic pass.

### Examples of Valid Local Declarations

```lucid
const compute () -> int = {
    @[deprecated("use newVec")]
    struct Vec2 { x float = 0.0  y float = 0.0 }

    @[inline]
    const add (a int)(b int) -> int = { return a + b }

    struct Point { x int = 0.0  y int = 0.0 }

    enum Color { Red = 0  Green = 1  Blue = 2 }

    const p Point = Point { x = 5, y = 5 }
    return add(p.x)(p.y);
}
```

### `if` / `else`

```lucid
if x > 0 {
    log("positive");
} else if x < 0 {
    log("negative");
} else {
    log("zero");
}
```

#### If / Else Narrowing

The compiler applies **type narrowing** inside branches based on the condition.

**Standard Narrowing — Inside the Block:**

When the condition checks a nullable variable, the compiler narrows its type inside the then-branch:

```lucid
const a int? = getValue();

if a != nil {
    -- a is int here, not int?
    const x int = a + 1;    -- OK
}
-- a is still int? here
```

**Inverse Narrowing — Early Exit Pattern:**

When a **standalone `if` with no `else`** contains a control flow exit (`return`, `break`, or `continue`), the compiler applies the **inverse** of the condition to the rest of the enclosing scope.

> [!WARNING]
> Inverse narrowing only applies to standalone `if` — no `else`
> The moment an `else` or `else if` is present, the compiler cannot guarantee which branch ran. The exit may have come from the `if` branch or never fired at all. Inverse narrowing is therefore **not applied** after an `if-else` chain.
>
> ```lucid
>    -- VALID: standalone if — inverse narrowing applies
> if a == nil { return }
>    -- a is non-nullable here
>
>    -- INVALID: has else — inverse narrowing NOT applied after the chain
> if a == nil { return } else { log("not nil") }
>    -- a is still int? here
>
>    -- INVALID: chained else-if — inverse narrowing NOT applied after the chain
> if a == nil { return } else if b == nil { return }
>    -- a and b are still nullable here — compiler cannot know which branch ran
>```

The condition determines what gets narrowed and in which direction:

| Condition  | Inside block            | Rest of scope (inverse)     |
| ---------- | ----------------------- | --------------------------- |
| `a == nil` | `a` is `nil`            | `a` is non-nullable         |
| `a != nil` | `a` is non-nullable     | `a` is nullable (no change) |
| `not a`    | `a` is `nil` or `false` | `a` is non-nullable         |

```lucid
-- guard: exit on nil → rest of scope is non-nullable
if a == nil { return }    -- rest: a is int
if not a    { return }    -- rest: a is int  (if a is a boolean/nullable)

-- guard: exit on non-nil → no narrowing gained after exit
if a != nil { return }    -- rest: a is int? (unchanged)
```

**`or` at the top level — each sub-condition narrowed independently:**

When conditions are joined by `or`, the exit fires if ANY is true. The inverse is ALL negated — every sub-condition's inverse is safely applied:

```lucid
if a == nil or b == nil { return }
-- inverse: a != nil AND b != nil
-- rest: a is int, b is string — both narrowed

if a == nil or b == nil or c == nil { return }
-- rest: a, b, c all non-nullable
```

**`and` at the top level — narrowing is unsound, not applied:**

When conditions are joined by `and`, the exit fires only if ALL are true. The inverse is `or` — at least one condition is false, but the compiler cannot know which. Narrowing any single variable would be unsound:

```lucid
if a == nil and b == nil { return }
-- inverse: a != nil OR b != nil
-- only one is guaranteed non-nil — cannot narrow either safely
-- no narrowing applied when 'and' is at the top level
```

> [!TIP]
> Inverse narrowing patterns to know
>
> **Flat nil guards at the top of a function** — eliminates deeply nested blocks and makes preconditions visible at a glance:
>
> ```lucid
> const process (a int?)(b string?)(c User?) -> int = {
>     if a == nil or b == nil or c == nil { return -1 }
>    -- from here: a is int, b is string, c is User
>     return a + strLength(b) + c.id;
> }
>```
>
> **Loop body guards** — skip nil elements without nesting:
>
> ```lucid
> for _, item int? in items {
>     if item == nil { continue }
>    -- item is int for the rest of this iteration
>     process(item);
> }
>```
>
> **Stack multiple standalone guards instead of chaining else-if** — each guard independently narrows its variables:
>
> ```lucid
>    -- WRONG: chained else-if, no inverse narrowing after chain
> if a == nil { return } else if b == nil { return }
>
>    -- CORRECT: two standalone guards, both narrow independently
> if a == nil { return }
> if b == nil { return }
>    -- a is int, b is string here
>```

#### If Expression — Inline Form

```ebnf
if_expr         = 'if' expr '??' expr 'else' expr
```

`else` is **required** in expression form. Both branches must produce compatible types. `??` is the separator between condition and then-branch.

```lucid
const grade string = if score >= 60 ?? "pass" else "fail";

-- chained (right-associative)
const label string = if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive";
```

### `while` and `do`/`while`

```lucid
let i int = 0;
while i < 10 {
    i = i + 1;
}

do {
    i = i - 1;
} while i > 0;
```

### `for`

`for` has exactly two forms. **Both require an explicit type annotation on
every loop variable.** Lucid does not infer a loop variable's type from
context — not from a range's literal bounds, and not from a collection's
already-known element type. This matches **Variable Declaration**: a type is
always written, even where the answer looks unambiguous, because the compiler 
does not guess.

**Form 1 — range iteration.** A single variable receives the *current value*
at each step. There is no backing collection and therefore no index to expose
— a second variable here is a semantic error. The variable's type must be
numeric (`int`, `float`, and so on). Inclusivity of the end bound is
controlled by `range_op`, matching **Range Expressions**:

```lucid
for i int in 0..10  { io:printl(stringFromInt(i)) }    -- 0 through 10 inclusive
for i int in 0..<10 { io:printl(stringFromInt(i)) }    -- 0 through 9, end excluded
```

An optional trailing `..` *expr* sets the step. Without it the step is `1`:

```lucid
for i int in 0..10..2 { io:printl(stringFromInt(i)) }    -- 0, 2, 4, 6, 8, 10 — step of 2
```

**Rejected — two variables on a range** (the step loop has no index to give):

```lucid
for i int, v int in 0..10 { ... }    -- ERROR: Form 1 takes one variable; use Form 2 only with a collection
```

**Form 2 — collection iteration.** An index variable and a value variable
are both required. The index is always `int`; the value must match the
collection's element type. When the index is not needed it may be discarded
with `_`, but the comma and the value binding are still required — writing
only a single variable is a semantic error, because there is no way to tell
from context whether the programmer meant the index or the value:

```lucid
import std.array as arr

const nums [*]int = [1, 2, 3, 4, 5];

-- index and value
for i int, v int in nums {
    log(stringFromInt(i) + ": " + stringFromInt(v));
}

-- discard the index when it is not needed
for _, v int in nums {
    log(stringFromInt(v));
}
```

**Rejected — single variable on a collection** (ambiguous: index or value?):

```lucid
for v int in nums { ... }    -- ERROR: Form 2 requires both index and value; use 'for _, v int in nums' to discard the index
```

**Rejected — two variables on a range** (already shown above — step loops carry no index):

```lucid
for i int, v int in 0..10..2 { ... }    -- ERROR: step ranges use Form 1 (single variable only)
```

### `switch`

`switch` dispatches on a single value. Arms are matched top to bottom and the
first matching arm executes — fall-through is disabled by default. The compiler
emits a jump table where possible (integer and enum types), guaranteeing O(1)
dispatch.

**Enum exhaustiveness** — when the switched value is an enum type and no
`default` clause is present, the compiler errors on any missing variant. This
ensures all variants are explicitly handled:

```lucid
enum Direction { North = 0  East = 1  South = 2  West = 3 }

-- exhaustive: all 4 variants covered, no default needed
switch dir {
    case Direction.North: { moveUp() }
    case Direction.South: { moveDown() }
    case Direction.East:  { moveRight() }
    case Direction.West:  { moveLeft() }
}

-- missing variant: compile error
switch dir {
    case Direction.North: { moveUp() }
    case Direction.South: { moveDown() }
    -- ERROR: Direction.East and Direction.West not covered, no default clause
}
```

**`default` clause** — catches all unmatched cases and suppresses
exhaustiveness errors. Valid on any switch, not just enum types:

```lucid
switch statusCode {
    case 200: { handleOk() }
    case 404: { handleNotFound() }
    case 500: { handleServerError() }
    default:  { handleUnknown() }
}
```

**Multiple values per case, separated by `,`** — one arm matches any of
several values:

```lucid
switch dir {
    case Direction.North, Direction.South: { moveVertical() }
    case Direction.East,  Direction.West:  { moveHorizontal() }
}
```

**Literal ranges** — a case may match an inclusive or exclusive range of
literal values, using the same `range_op` as **Range Expressions**:

```lucid
switch score {
    case 90..100:  { log("A") }
    case 80..<90:  { log("B") }
    case 70..<80:  { log("C") }
    default:       { log("F") }
}
```

Ranges combine with comma-separated values in the same arm:

```lucid
switch n {
    case 0, 1..9:   { log("single digit or zero") }
    case 10..<100:  { log("two digits") }
    default:        { log("large") }
}
```

**Primitive types** — `switch` works on any integer type, `bool`, `char`,
`string`, and enum. It does not work on struct, array, float, or function types:

```lucid
switch ch {
    case 'a', 'e', 'i', 'o', 'u': { log("vowel") }
    default:                      { log("consonant") }
}
```

#### Rejected Case Values

`case_value` is deliberately narrow — a bare literal or an enum variant, never
a general `expr`. `switch` dispatches on **matching a value**, not on
**evaluating a condition** — that distinction is what keeps a jump table
possible at all (see the O(1) dispatch note above), and it is what `if`/`else
if` is for when the branch genuinely depends on a computed condition rather
than a fixed value:

```lucid
-- INVALID — a comparison is a condition, not a value:
switch x {
    case a < b: { ... }    -- ERROR: case_value cannot be a binary_expr
}

-- INVALID — a fallback expression is not a value either:
switch x {
    case err ?? something: { ... }    -- ERROR: case_value cannot be a fallback_expr
}

-- INVALID — a range with variable bounds is still a computed value, not a literal:
switch n {
    case lo..hi: { ... }    -- ERROR: range bounds in a case_value must be literals
}

-- CORRECT — conditions belong in if/else if, not switch:
if a < b {
    ...
} else if x == err {
    ...
}
```

This is the same narrowing `case_value` already applies to a single literal
or enum variant — a comparison or a `??` fallback simply never appears in the
`case_value` production at all, so the compiler rejects them at parse time,
not as a later semantic check.

---

## Expressions

```ebnf
expr            = literal
                | IDENTIFIER
                | call_expr
                | index_expr
                | field_expr
                | module_expr
                | unary_expr
                | binary_expr
                | func_literal
                | struct_literal
                | array_literal
                | pipeline_expr
                | compose_expr
                | fallback_expr
                | generic_expr
                | range_expr
                | '(' expr ')'

literal         = INT_LIT | FLOAT_LIT | STRING_LIT | CHAR_LIT | BOOL_LIT | 'nil' | 'err'

call_expr       = expr '(' [ arg_list ] ')'
                  (* single argument group — chain for curried calls: f(a)(b) *)
arg_list        = expr { ',' expr }

index_expr      = expr '[' expr ']'                    (* single-element access *)
                | expr '[' [ expr ] range_op [ expr ] ']'  (* slice — see Slice Expressions *)

range_expr      = expr range_op expr
                  (* a standalone range value — e.g. for loop iteration *)

range_op        = '..'      (* inclusive end *)
                | '..<'     (* exclusive end *)

field_expr      = expr '.' IDENTIFIER           (* struct field access — may be l-value *)

module_expr     = IDENTIFIER ':' IDENTIFIER     (* module member access — never l-value,
                                                    at any '.field' depth — see
                                                    Depth of the Read-Only Guarantee *)
                | IDENTIFIER ':' call_expr      (* module function call *)
                | IDENTIFIER ':' generic_expr   (* module generic function call *)

unary_expr      = ( '-' | 'not' | '~' ) expr

binary_expr     = expr binary_op expr
binary_op       = '+' | '-' | '*' | '/' | '%' | '**'
                | '==' | '!=' | '<' | '<=' | '>' | '>='
                | 'and' | 'or'
                | '&' | '|' | '^' | '<<' | '>>'

func_literal    = param_group { param_group } '->' type block
                  (* anonymous function — Form 2 *)
                | param_group '->' func_type block
                  (* anonymous function — Form 1 *)

struct_literal  = IDENTIFIER '{' { field_init } '}'
                | IDENTIFIER '<' type_arg { ',' type_arg } '>' '{' { field_init } '}'
field_init      = IDENTIFIER '=' expr

array_literal   = '[' [ expr { ',' expr } ] ']'

generic_expr    = IDENTIFIER '<' type_arg { ',' type_arg } '>' '(' [ arg_list ] ')'
```

### Range Expressions

A range is a `start range_op end` value. It appears in three positions:
range iteration in `for` (see **`for`**), a slice bound (see **Slice
Expressions**, under **Arrays**), and a `switch` case value (see **Literal
ranges**, under **`switch`**). The two range operators differ only in
whether the end is included:

```lucid
0..10    -- inclusive: 0, 1, 2, ..., 10
0..<10    -- exclusive: 0, 1, 2, ..., 9
```

In `for` and in a slice bound, `start` and `end` may be arbitrary
expressions. In a `switch` case, both bounds must be **literals** — this
matches `case_value`'s general restriction to literals and enum variants
(see **Rejected Case Values**, under **`switch`**), since a `switch` case
dispatches on a matched value, never a computed condition:

```lucid
for i int in 0..count { ... }    -- OK: count is an arbitrary expression
const s [_]int = nums[lo..hi];    -- OK: lo, hi are arbitrary expressions

switch score {
    case 90..100: { log("A") }    -- OK: both bounds are literals
}

switch score {
    case lo..hi: { ... }    -- ERROR: case range bounds must be literals
}
```

A range is not a standalone collection value with its own type — it only
appears in the three positions above. Writing a bare range anywhere else
(e.g. `const r = 0..10`) is a compile error; there is no
general-purpose range type to assign it to.

### Logical Operators

`and`, `or`, and `not` are not restricted to `bool` — they accept a value of
**any type** and coerce it to a truth value first. The result of `and`, `or`,
and `not` is always `bool`.

**Truthiness rule:**

- **Non-nullable, non-fallible type** — always truthy. The type itself
  guarantees the value can never be `nil` or `err`, so this is a
  **compile-time** fact; no runtime check is generated.
- **Nullable type** `T?` — truthy unless the value
  currently holds `nil`. This is a **runtime** check, since the value's
  nilness can only be known when the expression executes.
- **Fallible type** (`T!`) — truthy unless the value currently holds `err`.
  Also a **runtime** check, for the same reason.
- **Nullable and fallible type** (`T?!`) — truthy unless the value currently
  holds `nil` **or** `err`. Either sentinel coerces to false.

Using `and`/`or`/`not` to coerce a fallible or nullable-and-fallible value is
a truthiness check only — it is **not** a narrowing operation. `if x { }`
tells you whether `x` is currently `T`, but does not let the compiler treat
`x` as plain `T` inside the block the way `if x != err { }` does (see
**Narrowing a Fallible Value**). Prefer explicit `== err` / `== nil` guards
when the block needs to use the underlying value.

`and`, `or`, and `not` use the existing `binary_op` and `unary_expr`
productions — no new grammar is introduced, only the typing/coercion rule
below.

```lucid
const name  string = "alice";
const user  User?  = findUser(1);
const conn  Connection! = openConnection();

if name and user { }
-- name: string is non-nullable, non-fallible → always true, compile time
-- user: User? is nullable → runtime check, true unless user == nil

if conn { }    -- runtime: true unless conn == err
-- NOTE: conn is still Connection! inside this block — coercion is not
-- narrowing. Use 'if conn != err { }' to narrow conn to plain Connection.

if not user { return }    -- runtime: true only when user is nil
if not conn { return }    -- runtime: true only when conn is err
```

`and` and `or` short-circuit. The right-hand operand is not evaluated unless
needed:

```lucid
if getUser() != nil and validate(getUser()) { }    -- short-circuit
if has(cache, key) or expensiveLoad(key) { }    -- short-circuit
```

`not` negates the coerced truth value of its operand:

```lucid
if not isValid { }
if not x { }    -- x is nullable: nil treated as false, not flips to true
```

> [!NOTE]
> Coercing a non-nullable operand is never a runtime branch — the compiler
> already knows the answer. Writing `and`/`or`/`not` against a non-nullable
> operand is legal but the check folds away entirely; it is only useful when
> mixed with a nullable operand in the same expression, as in `name and user`
> above.

### Bitwise Operators

Integer types only. `&` and `|` are bitwise AND/OR (not logical — those use `and`/`or` keywords). This avoids ambiguity with `&` (reference operator).

| Operator | Name                |
| -------- | ------------------- |
| `&`      | bitwise AND         |
| `\|`     | bitwise OR          |
| `^`      | bitwise XOR         |
| `~`      | bitwise NOT (unary) |
| `<<`     | left shift          |
| `>>`     | right shift         |

```lucid
const flags   uint32 = 0xFF00;
const mask    uint32 = 0x0F0F;
const result  uint32 = flags & mask;    -- 0x0F00
const merged  uint32 = flags | mask;    -- 0xFF0F
const inv     uint32 = ~flags;    -- bitwise NOT
const shifted uint32 = 1 << 4;    -- 16
```

---

## Pipeline Operator `|>`

A pipeline passes the result of one expression as the first argument to the
next step, executing left to right at runtime.

```ebnf
pipeline_expr   = expr { '|>' pipeline_step }

pipeline_step   = expr                          (* single-group function or partial application *)
                | expr '(' arg_list ')' '!'     (* argument pack — upstream injected as first arg *)
                | func_literal                  (* anonymous function *)
```

```lucid
const result [*]string =
    [1, 2, 3]
    |> map<int, string>(stringFromInt)!;
    |> filter<string>(isNonEmpty)!;
    |> map<string, string>(trim)!;
```

### Argument Pack `!`

`fn(args)!` is not a function call — `!` marks an intentionally incomplete
argument list. The upstream value is injected as the **first** argument when
`|>` fires:

```lucid
const scale (factor float)(v float) -> float = { return v * factor }

-- without !: scale(2.0) is a complete partial application — no slot for upstream
42.0 |> scale(2.0);    -- ERROR: upstream has no parameter to fill

-- with !: upstream fills the first group
42.0 |> scale(2.0)!;    -- calls scale(42.0)(2.0) → 84.0
```

### Curry Functions in Pipelines

`|>` fills exactly one parameter group. A curried function with remaining
unfilled groups is an error as a pipeline step. Pre-apply first:

```lucid
const clamp (lo int)(hi int)(v int) -> int = {
    if v < lo { return lo }
    if v > hi { return hi }
    return v;
}

42 |> clamp    -- ERROR: 3 groups — upstream fills (lo), (hi) and (v) unresolved
42 |> (v int) -> int { return clamp(0)(100)(v) }    -- OK: wrap in anonymous function

-- CORRECT: pre-apply to a single-group function
const clamp0to100 (v int) -> int = clamp(0)(100);
42 |> clamp0to100    -- OK → 42
150 |> clamp0to100    -- OK → 100
```

### Generic Functions in Pipelines

Generic functions must be instantiated with explicit type arguments at the
pipeline step site. An uninstantiated generic is a compile error:

```lucid
const identity<T> (v T) -> T = { return v }
const map<T, U>   (v T)(f (T) -> U) -> U = { return f(v) }

42     |> identity<int>    -- OK → 42
42     |> identity    -- ERROR: uninstantiated generic
42     |> map<int, string>(stringFromInt)!;    -- OK → "42"
"hello" |> map<string, int>(length)!;    -- OK → 5

-- chaining generic steps
const result string =
    42
    |> identity<int>
    |> map<int, string>(stringFromInt)!;
    |> map<string, string>(trim)!;
```

---

## Composition Operator `+>`

`+>` wires functions together at compile time, producing a new function without
executing anything. The output type of the left operand must exactly match the
input type of the right operand. **Both operands must have exactly one
parameter group** — curry functions are forbidden on either side (see
**Curry functions are forbidden on both sides of `+>`**, below); this keeps
every composition's shape fully determined by the two operands' single
input/output types, with no leftover argument groups whose origin would be
ambiguous to a reader.

```ebnf
compose_expr    = expr '+>' expr     (* f +> g: apply f then g, always left-to-right
                                         both f and g must have exactly one
                                         param group — see below *)
```

```lucid
const f (a int)    -> string = { ... }
const g (s string) -> bool   = { ... }

const h   (a int) -> bool = f +> g;    -- OK: f returns string, g takes string
const bad          = g +> f;    -- ERROR: g returns bool, f takes int

-- chain three or more
const process (raw string) -> bool = validate +> transform +> render;
```

### Generic Functions and `+>`

`+>` is where generic functions are most powerful. Instantiated at the
composition site, a generic function acts as a universal adapter between any
two compatible types:

```lucid
const toString<T>  (v T)      -> string = { ... }
const parseFloat   (s string) -> float  = { ... }
const double       (x float)  -> float  = { return x * 2.0 }

-- int → string → float → float
const intToDoubled (x int) -> float =
    toString<int> +> parseFloat +> double

intToDoubled(42);    -- "42" → 42.0 → 84.0
intToDoubled(10);    -- "10" → 10.0 → 20.0
```

**Generics reduce boilerplate across type combinations:**

```lucid
-- without generics: one wrapper per type combination
const pipeInt   (v int)   -> string = validateInt   +> intToStr   +> trim;
const pipeFloat (v float) -> string = validateFloat +> floatToStr +> trim;
const pipeBool  (v bool)  -> string = validateBool  +> boolToStr  +> trim;

-- with generics: instantiate at composition site
const pipeInt   (v int)   -> string = validateInt   +> toString<int>   +> trim;
const pipeFloat (v float) -> string = validateFloat +> toString<float> +> trim;
const pipeBool  (v bool)  -> string = validateBool  +> toString<bool>  +> trim;
```

**Curry functions are forbidden on both sides of `+>`** — every operand,
left or right, must have exactly one parameter group. `+>` only ever checks
that the left operand's output type matches the right operand's input type;
nothing about that check looks at whether either operand has *further*
curried groups beyond the one being matched. A curried operand on the right
is just as unpredictable as one on the left — the composed function would
end up needing extra argument groups that don't trace cleanly back to either
original function, which is exactly the kind of result that's hard to
predict from reading the composition alone. Both sides are pre-applied down
to a single group before composing:

```lucid
const clamp   (lo int)(hi int)(v int) -> int = { ... }
const scaleBy2 (v int)                -> int = { return v * 2 }

const pipeline = clamp +> scaleBy2;    -- ERROR: clamp has 3 groups (left side)

const validate (a int) -> string = { ... }
const checkLen (s string)(extra int) -> bool = { ... }

const bad = validate +> checkLen;    -- ERROR: checkLen has 2 groups (right
                                              -- side) — even though validate's
                                              -- output type matches checkLen's
                                              -- first group's input, the composed
                                              -- function would still need a second
                                              -- argument group ('extra') with no
                                              -- clear origin

-- CORRECT: pre-apply every operand to a single group first
const clamp0to100 (v int) -> int = clamp(0)(100);
const pipeline    (v int) -> int = clamp0to100 +> scaleBy2;

const checkLenWith5 (s string) -> bool = (s string) -> bool { return checkLen(s)(5) }
const ok            (a int) -> bool = validate +> checkLenWith5;

pipeline(150);    -- clamp → 100, scale → 200
pipeline(50);    -- clamp → 50,  scale → 100
pipeline(-10);    -- clamp → 0,   scale → 0
```

**Nullable operands are forbidden** in composition:

```lucid
const transform (v int) -> int = nil;

const pipeline = transform +> scaleBy2;

-- CORRECT: guard before composing
if transform != nil {
    const pipeline (v int) -> int = transform +> scaleBy2;
}
```

---

## Result Type and Error Handling

Lucid treats errors as values, symmetric with how nullability is treated. A
function that can fail marks its return type with `!`. A value of a fallible
type holds exactly one of two things at any moment: a plain `T`, or the bare
sentinel `err`. This mirrors `T?`, which holds either a plain `T` or the bare
sentinel `nil`. Both are resolved the same way — narrowing with `if` — and
both are **inert** until narrowed: the compiler forbids using an un-narrowed
fallible or nullable value as a plain `T`.

There are no exceptions and no registered handlers. The one narrow exception
is the runtime **panic** (see **Runtime Panics** below), reserved for
primitive operations the type system does not track as fallible — division
and indexing being the two built-in cases. Even there, the only way to
prevent termination is `??` written at the risky expression itself, never a
handler somewhere else up the call stack.

### Fallible Types

`!` is a postfix fallible, not an infix operator. It carries no payload —
there is no separate error type to declare. A function either succeeds with
`T` or fails with the bare `err` sentinel:

```lucid
int!    -- holds either int or err
string!    -- holds either string or err
User?!    -- holds either User, nil, or err — three possible states
```

### `!` on Array Types

`!` binds to the **element type** of an array, never to the array itself —
the same rule already established for `?`:

```lucid
[*]int!    -- array of fallible int — each element is independently
        -- int!, narrowed individually when read
```

### `T?!` — Combined Nullable and Fallible

`T?!` is the single canonical spelling for "may be absent, may have failed."
The reverse order, `T!?`, does not parse — there is exactly one way to write
this combination, so source code never has to choose between equivalent
forms:

```lucid
const x User?! = riskyLookup();    -- OK: x holds User, nil, or err
const y User!? = riskyLookup();    -- ERROR: '!?' is not valid order, use '?!'
```

A `T?!` value is a genuine three-state value. Narrowing must rule out both
sentinels before the plain `T` is usable.

### Narrowing a Fallible Value

`err` narrows exactly like `nil` — using the same `if`/standalone-guard
machinery already defined for nullable types (see **If / Else Narrowing**).
No new control-flow construct is introduced.

**Standard narrowing — inside the block:**

```lucid
const x int! = riskyOp();

if x != err {
    -- x is int here, not int!
    const total int = x + 1;    -- OK
}
-- x is still int! here
```

**Inverse narrowing — standalone guard with an exit:**

```lucid
const process (id int) -> int = {
    const x int! = riskyOp(id);
    if x == err { return -1 }
    -- x is int here for the rest of the function
    return x + 1;
}
```

The narrowing table extends with `err` alongside `nil`:

| Condition  | Inside block        | Rest of scope (inverse)     |
| ---------- | ------------------- | --------------------------- |
| `a == nil` | `a` is `nil`        | `a` is non-nullable         |
| `a != nil` | `a` is non-nullable | `a` is nullable (no change) |
| `a == err` | `a` is `err`        | `a` is non-fallible         |
| `a != err` | `a` is non-fallible | `a` is fallible (no change) |

**`T?!` — narrowing both sentinels.** Per the existing `or`-narrowing rule,
joining both checks with `or` narrows both independently once the guard
exits:

```lucid
const process (id int) -> int = {
    const x User?! = riskyLookup(id);
    if x == nil or x == err { return -1 }
    -- x is User here — neither nil nor err remain possible
    return x.id;
}
```

Stacked standalone guards work identically to the existing nullable
convention — prefer them over chained `else if` for the same soundness
reasons already established:

```lucid
-- CORRECT: standalone guards, each narrows independently
const a int?! = riskyOp();
const b string?! = riskyOp2();
if a == nil or a == err { return }
if b == nil or b == err { return }
-- a is int, b is string here
```

> [!WARNING]
> Exactly as with `nil`, inverse narrowing for `err` only applies after a
> **standalone `if` with no `else`** that exits. The moment an `else` or
> `else if` is present, the compiler cannot guarantee which branch ran, and
> narrowing is not applied. See **If / Else Narrowing** for the full rule —
> it applies identically here, with `err` and `nil` checks treated the same
> way `and`/`or` combinations are already analyzed for soundness.

### Forbidden Operations on an Un-narrowed Fallible Value

A `T!` or `T?!` value is inert until narrowed. The following are all compile
errors:

```lucid
const x int! = riskyOp();

x + 1    -- ERROR: cannot apply '+' to un-narrowed int!
const y = x;    -- ERROR: cannot assign un-narrowed int!
io:printl(x);    -- ERROR: cannot pass un-narrowed int! as argument
x.field    -- ERROR: cannot access field on un-narrowed int!
x:method();    -- ERROR: cannot call method on un-narrowed int!
x |> double    -- ERROR: cannot pipe un-narrowed int!
x == 5    -- ERROR: cannot compare un-narrowed int! to int
```

`x == err` (and, for `T?!`, `x == nil`) are the only comparisons legal on an
un-narrowed value — they are the narrowing operations themselves, not uses of
the underlying `T`.

### `??` Fallback

`??` triggers its right-hand side when the left-hand side is `nil`, `err`, or
both — covering nullable and fallible values with one operator. The
right-hand side may be a single expression, or a block.

The result type of `x ?? rhs` is **not always plain `T`** — it is whatever
type `rhs` actually produces, checked against `x`'s own type:

- If `rhs` is a plain `T` expression, the result is `T` — this is the common
  case, fully resolving the failure.
- If `rhs` is a block, the block's final expression determines the result
  type. A block is free to return `err` again (re-raising, after doing
  something in between — a retry, a log, a side effect) rather than fully
  resolving the value. In that case the result of the whole `?? ` expression
  is still `T!`, not `T` — the failure is preserved, not silently erased.
- The compiler checks the block's result type against `x`'s declared type the
  same way it checks any other assignment: a `T!` left-hand side accepts a
  block that produces `T` **or** `err`; a plain non-fallible `T` left-hand
  side (where `??` is closer to dead code, but still legal) only accepts a
  block that produces exactly `T`.

```ebnf
fallback_expr   = expr '??' expr
                | expr '??' block
                  (* result type = type of rhs, checked against lhs's type:
                     lhs T  (non-fallible): rhs must produce exactly T
                     lhs T! : rhs may produce T or err
                     lhs T? : rhs may produce T or nil
                     lhs T?!: rhs may produce T, nil, or err
                     a block's result is its final expression, or 'return' *)
```

```lucid
-- common case: block fully resolves to plain T
const x int = (10 / d) ?? {
    system:logError("division failed");
    return -1;
}
-- x is int — the block's last expression/return is plain int

-- block re-raises: result stays int!, not int
const y int! = (10 / d) ?? {
    const retryDivisor int = recoverDivisor(d);
    if retryDivisor == 0 { return err }    -- still no valid divisor — re-raise

    const retried int! = 10 / retryDivisor;    -- still runtime-checked
    if retried == err { return err }
    return retried;
}
-- y is int! here — caller of y still must narrow it; the failure was not
-- silently discarded, only retried
```

```lucid
-- nullable
const a int? = nil;
const b int  = a ?? 0;

-- fallible
const c int! = riskyOp();
const d int  = c ?? 0;    -- err discarded, d = 0

-- nullable and fallible together
const e User?! = riskyLookup();
const f User   = e ?? User { id = 0  name = "guest"  email = "" }

-- never triggers: lhs is plain int
const g int = getValue();
const h int = g ?? 0;    -- always g

-- block form: multiple statements, then a value
const i int = riskyOp() ?? {
    system:logError("riskyOp failed, using default");
    return -1;
}
```

`??` is also the separator in inline `if` expressions (see **If Expression —
Inline Form**: `if_expr = 'if' expr '??' expr 'else' expr` — there is no
`then` keyword; `??` itself separates the condition from the then-branch).
The two roles never collide: as an infix binary operator, `??` sits in the
standard expression precedence table; in the `if_expr` production, it
occupies a fixed grammatical slot immediately after the condition, so the
parser never has to choose between the two readings inside a single
production.

> [!NOTE]
> Use `??` when a sensible default exists and the failure does not need to be
> distinguished from absence. Use an explicit `if x == err { ... }` guard when
> the function needs to react differently — log, retry, propagate — to a
> failure versus a `nil`.

### A Single `??` Cannot Branch on Which Sentinel Fired

A single `??` always runs the same right-hand side regardless of whether the
failure was `nil` or `err` — there is no way to ask, inside one `??`, "which
sentinel was this" the way a compiler with several `catch` clauses per
exception type can dispatch on the exception's type. There is no dedicated
multi-branch syntax for `??` — only `expr '??' expr` and `expr '??' block`
exist (see **`??` Fallback**).

A tempting but mistaken way to reach for per-sentinel handling is writing two
blocks back-to-back, expecting the first to handle `nil` and the second to
handle `err`:

```lucid
-- MISLEADING — does not mean "first block for nil, second for err":
const x int = riskyOp();
    ?? { return -1 }
    ?? { return -2 }
```

This parses and typechecks, but not with that meaning. The first `?? { return
-1 }` already fully resolves `x` to plain `int` regardless of which sentinel
fired — `nil` and `err` both take this same branch. The second `?? { return
-2 }` is then dead code: its left-hand side is already plain `int`, never
`nil` or `err`, so it can never trigger.

**Chaining separate `??` expressions is fine**, and is the idiomatic way to
layer fallbacks — each one only has to decide "is this still unresolved,"
not "which sentinel was it." Give the intermediate result its own declaration with
an explicit type, so it's visible at a glance whether the next `??` is live
or dead — this is exactly the check the MISLEADING example above skipped:

```lucid
const x int! = riskyOp();

const step1 int! = x ?? {
    const retried int! = retryOp();
    if retried == err { return err }
    return retried;
}
-- step1 is explicitly int! — the block re-raised, so it did NOT fully
-- resolve x. This makes the next ?? visibly live, not dead:
const step2 int = step1 ?? -1;    -- fully resolves whatever remains
```

Avoid writing this as one inline chain (`x ?? { ... } ?? -1`) even though it
is grammatically legal — without the intermediate `const step1 int!`
spelled out, a reader has to mentally execute the first block to know whether
the second `??` is live or dead, which is precisely the trap the MISLEADING
example above demonstrates. Prefer giving a chained intermediate its own
typed declaration.

> [!NOTE]
> Inline chaining is a style choice, not a forbidden construct. The grammar
> does not special-case `expr '??' block` to reject a further `??` after it —
> doing so would not close the underlying problem anyway, since the identical
> readability trap exists when chaining through an ordinary function call
> instead of a block (`riskyOp() ?? fallbackOp() ?? -1` has the same
> live-or-dead ambiguity, and there is no clean syntactic line between "a
> call that happens to return a fallible type" and "a block"). The
> intermediate-declaration convention above is a recommendation, not a rule the
> compiler enforces.

The block after `??` is an ordinary `block` — it can contain any statement,
including `if` or `switch`, exactly like a function body. This is not the
same as branching on which sentinel triggered the `??`: the `??` itself still
only ever knew "unresolved, run this block once" — the `switch` inside is
dispatching on some other value the block computes, not on `nil` vs `err`:

```lucid
-- (assume the following lives inside a function body, as with all
-- 'return'-containing snippets in this section)
 recoveryCode (d int) -> int = { ... }

 d int = getDivisor();

 result int = (10 / d) ?? {
    switch recoveryCode(d) {
        case 1: { return -1 }
        case 2: { return -2 }
        default: { return 0 }
    }
}
```

Each `??` in the chain still only ever sees "resolved or not" — not which
sentinel. If the handling genuinely needs to differ by sentinel, narrow
explicitly with `if` and dispatch to ordinary functions instead — `switch`
does not apply here for struct-typed values like `User` (see **Primitive
types** under **`switch`**), so this is `if`/`else if` over the narrowing
checks already established for `nil` and `err`:

```lucid
 handleAbsent (lookup User?!) -> int = {
    system:logError("value was absent");
    return -1;
}

 handleFailure (lookup User?!) -> int = {
    system:logError("operation failed");
    return -2;
}

 x User?! = riskyLookup();

 result int =
    if x == nil ?? handleAbsent(x)
    else if x == err ?? handleFailure(x)
    else x.id
```

> [!NOTE]
> Whether `x` narrows to plain `User` inside the final `else x.id` branch
> follows the same rule as **Standard Narrowing — Inside the Block** for
> nested `if`/`else if` chains generally; this example passes `x` itself into
> the handler functions to stay independent of that question.

For a primitive-typed value (int, bool, char, string, enum), the same
dispatch can use `switch` instead, since `nil` and `err` are valid
`case_value`s:

```lucid
 handleAbsentCode  () -> int = { system:logError("absent")  return -1 }
 handleFailureCode () -> int = { system:logError("failed")  return -2 }

 code int?! = riskyParse();

let result int = 0;
switch code {
    case nil: { result = handleAbsentCode() }
    case err: { result = handleFailureCode() }
    default:  { result = 0;    -- see note below on narrowing in 'default' }
}
```

> [!NOTE]
> Separately: whether `code` narrows to plain `int` inside `default` follows
> the same rule referenced above for `if`/`else if` chains; until that is
> settled, do not rely on using `code` directly inside `default` — narrow it
> explicitly first with a guard if the plain value is needed there.

This keeps `??` simple — one operator, one fallback — and keeps branching
logic in the same place all other branching logic lives, `if`/`switch`,
rather than growing a parallel mini-syntax inside `??` itself.

### Runtime Panics

Some primitive operations can fail in a way no declared `!` describes —
integer division by a divisor that turns out to be zero, or indexing an array
past its bound. These are not user-declared fallible functions; they are
built-in operators on plain, non-fallible types (`int / int`, `[*]T[int]`).

Lucid does not silently produce a wrong value in these cases, and it does not
require every arithmetic or index expression to carry `!` in its type either
— that would make ordinary arithmetic unusably verbose. Instead:

- If lucid can **prove** the operation always succeeds — dividing by a
  nonzero literal, or indexing a fixed-size array (`[N]T`) with a literal
  index that is provably less than `N` — the expression types as plain `T`
  and no check exists at runtime. A literal index into a slice (`[_]T`) or
  dynamic array (`[*]T`) does **not** qualify, since their length is not part
  of the type and cannot be proven at compile time.

> [!NOTE]
> **Interpreter Mode:** The interpreter always performs runtime checks for
> division and indexing operations. The compile-time proof optimization
> (eliminating the check entirely for provably-safe literals) will be available
> in the future compiled backend.
- If lucid **cannot prove** this (e.g. dividing by a variable, indexing
  with a runtime-computed index, or indexing a slice/dynamic array at all),
  the operation is checked at runtime. Left unhandled, a failing check
  **panics**: the program terminates immediately with a diagnostic. Attaching
  `??` to the expression converts the panic into ordinary control flow
  instead — the fallback runs and the program continues.

```lucid
const a int = 10 / 2;    -- OK: divisor is a nonzero literal, no check
const b int = 10 / d;    -- d is a variable divisor — runtime-checked

const c int = 10 / d ?? -1;    -- checked; panic converted to -1 on failure
const e int = 10 / d;    -- checked; PANICS at runtime if d == 0
```

```lucid
const items [*]int = [1, 2, 3];
const x int = items[i];    -- i is runtime-computed — checked
const y int = items[i] ?? 0;    -- checked; panic converted to 0
```

> [!WARNING]
> A panic is unconditional program termination — there is no `catch`,
> `recover`, or handler registered elsewhere that can intercept it once it
> fires. The only way to prevent a panic is `??` at the expression that could
> trigger it. This is deliberately the opposite of exceptions: the recovery
> must be written at the point of risk, not somewhere else up the call stack.
> A panic is reserved for primitive operations the type system does not track
> as fallible — user code should prefer declaring `!` and narrowing with `if`
> over relying on this fallback, since `!` keeps the failure visible in the
> function's signature for every caller.

### Producing a Failure

A function produces a failure by returning the bare `err` sentinel from any
branch. This is the only place a failure originates — there is no separate
"throw" or "raise" construct. Every `err` value anywhere in a running program
traces back to exactly one such `return err` site:

```lucid
const divide (a int, b int) -> int! = {
    if b == 0 {
        return err;
    }
    return a / b;
}
```

### Propagation

A function that calls a fallible function and wants to fail the same way
narrows the result explicitly with a guard, then returns `err` itself — there
is no implicit pass-through. A fallible value may not be returned directly
without narrowing first, even when the caller's own return type matches
exactly:

```lucid
const fetch (url string) -> string! = { ... }

const process (url string) -> string! = {
    const raw string! = fetch(url);
    if raw == err { return err }
    -- raw is string here
    return raw;
}
```

```lucid
-- INVALID: returning an un-narrowed fallible value, even with a matching
-- signature, is forbidden — the compiler cannot tell this apart from
-- forgetting to handle the failure
const badProcess (url string) -> string! = {
    const raw string! = fetch(url);
    return raw;    -- ERROR: cannot return un-narrowed string!
}
```

This guarantees every propagation point is visible as an explicit `if raw ==
err { return err }` line in the source — failures never travel through a
function silently, and there is no way to "accidentally" forward a failure
without writing the guard.

### Error Detail Without a Payload

Since `err` carries no data, a function that needs to communicate *why* it
failed does so with an ordinary out-of-band value — a logged message, a
package-level reporting function, or a struct returned through a different
mechanism (e.g. a `let` out-parameter, or a wrapping struct holding both a
status and a value). This keeps the type system simple: `!` only ever answers
"did this fail," and any richer detail is modeled with the same struct/enum
tools used everywhere else in the compiler, not folded into the fallible-type
syntax itself:

```lucid
enum FetchError {
    Network = 0
    Parse   = 1
    Timeout = 2
}

const fetch (url string)(lastError FetchError?) -> string! = {
    if not reachable(url) {
        lastError = FetchError.Network;
        return err;
    }
    return readBody(url);
}
```

### Error Handling in Pipelines

A fallible value cannot be a pipeline step directly — every step must be a
function, and narrowing is a statement, not an expression that fits inside a
pipeline. To narrow inside a pipeline, use an anonymous function as the final
step:

```lucid
const result string = dbFindUser(id);
    |> formatUser
    |> (v string!) -> string {
        if v == err { return "unnamed" }
        return v;
    }
```

`??` can also appear at the end of a pipeline directly when the failure does
not need to be distinguished from absence:

```lucid
const result string = fetchData(url);
    |> parseJson
    |> formatOutput
    ?? ""    -- any step that failed or returned nil: use ""
```

### Complete Example

```lucid
struct User {
    id    int
    name  string
    email string
}

enum DbError {
    NotFound       = 0
    ConnectionLost = 1
}

const dbFindUser (id int)(lastError DbError?) -> User?! = {
    if id < 0 {
        lastError = DbError.NotFound;
        return err;
    }
    return db:query(id);    -- returns User?, nil if not found
}

const formatUser (user User) -> string = {
    if user.name == "" { return "user has no name" }
    return user.name + " <" + user.email + ">";
}

const getFormattedUser (id int) -> string = {
    let lastError DbError? = nil;
    const found User?! = dbFindUser(id)(lastError);

    if found == err {
        system:logError("lookup failed: " + string(lastError));
        return "guest";
    }
    -- found is User? here — err ruled out, nil still possible

    const user User = found ?? User { id = 0  name = "guest"  email = "" }

    return user |> formatUser;
}
```

---

## Arrays

An array is a flat, ordered collection of values of a single type. Lucid has
three array shapes, distinguished by the size slot:

```ebnf
array_type  = '[' array_size ']' type

array_size  = '*'       (* owned heap array — Lucid allocates and owns the memory *)
            | '_'       (* slice — a borrowed view into another array, Lucid does not own *)
            | INT_LIT   (* fixed-size stack array — size known at compile time *)
```

```lucid
const owned  [*]int    = [1, 2, 3];    -- heap array, Lucid owns memory
const view   [_]int    = owned;    -- slice — borrows from owned
const fixed  [3]float  = [1.0, 2.0, 3.0];    -- stack array, size fixed at compile time
```

**The `?` and `!` annotation is applied to the element type not the whole array.**
Use an empty array when you need to tell if the operation on the array has failed 

```lucid
[*]int?    -- array of nullable int
[*]int!    -- array of fallible int
```

### Array Literals

```lucid
const empty  [*]int    = [];
const nums   [*]int    = [1, 2, 3, 4, 5];
const matrix [3][3]float = [
    [1.0, 0.0, 0.0],
    [0.0, 1.0, 0.0],
    [0.0, 0.0, 1.0]
]
```

### Element Access and Index

```lucid
const first  int = nums[0];
const last   int = nums[4];

-- index out of bounds is a runtime error
-- no implicit wrapping or clamping
```

### No Built-in Array Functions

Lucid does not provide built-in methods or operators for array manipulation —
there is no `.push`, `.sort`, `.map`, `.filter`, or similar attached to the
array type. This is consistent with Lucid's design: **behavior comes from
functions, not from types**.

All array manipulation is done through the standard library, which provides
plain functions that accept the array and a user callback where needed:

```lucid
import std.array as arr

const nums  [*]int = [3, 1, 4, 1, 5, 9, 2, 6];

-- sorting — user provides the comparison callback
const sorted [*]int = arr:sort<int>(nums)(
    (a int, b int) -> int { return a - b }    -- ascending
)

-- mapping — user provides the transform callback
const doubled [*]int = arr:map<int, int>(nums)(
    (v int) -> int { return v * 2 }
)

-- filtering — user provides the predicate callback
const evens [*]int = arr:filter<int>(nums)(
    (v int) -> bool { return v % 2 == 0 }
)

-- reducing — user provides the accumulator callback
const sum int = arr:reduce<int, int>(nums)(0)(
    (acc int, v int) -> int { return acc + v }
)

-- searching — user provides the predicate
const found int? = arr:find<int>(nums)(
    (v int) -> bool { return v > 4 }
)
```

Since the std array library functions are plain curried functions, they compose
naturally with pipeline and composition:

```lucid
import std.array as arr

const result [*]string =
    [3, 1, 4, 1, 5, 9, 2, 6]
    |> arr:filter<int>(isPositive)!;
    |> arr:sort<int>((a int, b int) -> int { return a - b })!;
    |> arr:map<int, string>(stringFromInt)!;
```

### Slice Expressions

A slice expression — `expr[start range_op end]`, `expr[start range_op]`, or
`expr[range_op end]` — produces a `[_]T` borrowed view over a contiguous
range of an existing array, without copying. It reuses the same `range_op`
as **Range Expressions**, so the same `..` / `..<` end-inclusivity rule
applies:

| Operator | End bound | Example on `[10, 20, 30, 40, 50]`          |
| -------- | --------- | ------------------------------------------ |
| `..`     | inclusive | `nums[1..3]` → `[20, 30, 40]` (3 elements) |
| `..<`    | exclusive | `nums[1..<3]` → `[20, 30]` (2 elements)    |

Either bound may be omitted — an omitted start defaults to `0`, an omitted
end defaults to the array's length:

```lucid
const nums [*]int = [10, 20, 30, 40, 50];

const sub    [_]int = nums[1..3];    -- [20, 30, 40]
const subEx  [_]int = nums[1..<3];    -- [20, 30]
const head   [_]int = nums[..<2];    -- [10, 20] — start defaults to 0
const tail   [_]int = nums[3..];    -- [40, 50] — end defaults to length
const all    [_]int = nums[..];    -- [10, 20, 30, 40, 50] — whole array
```

A slice expression's bounds are runtime-checked the same way a single-element
index is — see **Runtime Panics**. A start or end that falls outside the
array, or a start greater than the end, panics unless guarded with `??`:

```lucid
const bad [_]int = nums[1..99];    -- PANICS: end out of bounds
const ok  [_]int = nums[1..99] ?? [];    -- panic converted to an empty slice
```

### Slice Rules

A slice `[_]T` is a borrowed view — it does not own the underlying memory. The
backing array must outlive the slice:

```lucid
const data  [*]int = [1, 2, 3, 4, 5];
const view  [_]int = data;    -- borrows from data
const sub   [_]int = data[1..3];    -- elements at index 1, 2, 3

-- writing through a mutable slice modifies the backing array
let buf   [*]int = [0, 0, 0];
let window [_]int = buf;
window[0] = 42;    -- buf[0] is now 42
```

### Fixed Arrays

A fixed array `[N]T` is stack-allocated. Its size must be a compile-time
integer literal:

```lucid
const rgb   [3]uint8  = [255, 128, 0];
const mat4  [16]float = [
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0
]

-- pass as slice to functions that accept [_]T
const view [_]float = mat4;
```

---

## Value and Reference Semantics

Every type is either **owned** or **borrowed**. Bare `T` = owned, `&T` = borrowed.

### Owned Types — Copied on Assignment

| Type           | Syntax                           | Storage                         | On assignment                      |
| -------------- | -------------------------------- | ------------------------------- | ---------------------------------- |
| Primitives     | `bool` `int` `float` `char` …    | stack / inline                  | full copy                          |
| Enum           | `Direction.North`                | integer (`byte`/`short`)        | full copy                          |
| Fixed array    | `[N]T`                           | stack / inline                  | full element copy                  |
| Slice          | `[_]T`                           | fat pointer (`ptr + len + cap`) | copies view header — shares buffer |
| Dynamic array  | `[*]T`                           | heap-owned buffer               | full deep copy                     |
| String         | `string`                         | heap-owned sequence             | full deep copy                     |
| Struct         | `Vec2` `Player` …                | inline / stack                  | full deep copy                     |
| Named function | `add` `math:normalize`           | function pointer                | pointer copy                       |
| Closure        | `add(10)` `(x int) -> int { … }` | heap-allocated env              | copies reference to env            |

### Struct Deep Copy

Struct assignment always produces a fully independent value. Owned fields are cloned. Since references (`&T`) cannot be stored in structs (see Scoped Reference Rules below), structs consist entirely of owned fields and deep copies are always independent.

```lucid
struct Player {
    score  int    -- owned: cloned
    items  [*]string    -- owned: buffer deep-copied
}

const a Player = Player { score = 10, items = ["sword"] }
let b Player = a;
-- b.score and b.items are fully independent of a
```

### Borrowed Types — Scoped References (`&T`)

References (`&T`) allow sharing data without copying. They represent a safe borrowed view of an owned variable.

```lucid
const a Player = Player { … }

let ref &Player = a;    -- mutable shared reference
const rc &Player = a;    -- read-only shared reference
```

| Declaration            | Copies?     | Field mutation?       | Owner |
| ---------------------- | ----------- | --------------------- | ----- |
| `let b Player = a`     | ✅ deep copy | ✅ b's own fields      | `b`   |
| `let ref &Player = a`  | ❌ shared    | ✅ visible through `a` | `a`   |
| `const rc &Player = a` | ❌ shared    | ❌ read-only           | `a`   |

#### The Downward Flow Rule (Reference Scoping)

To guarantee memory safety and eliminate dangling pointers without using a Garbage Collector or a complex compile-time borrow checker, references (`&T`) are strictly scoped. They are allowed to flow *downward* (into nested calls), but never *upward or sideways*.

1. **No Struct Storage:** A struct field cannot have a reference type (e.g., `field &T` is a compile error).
2. **No Array/Slice Storage:** An array or slice cannot store reference types (e.g., `[*]&T` or `[_]&T` is a compile error).
3. **No Reference Returns:** A function cannot return a reference type (e.g., `-> &T` is a compile error).

As a result, a reference (`&T`) can only exist in two places:
*   As a **function parameter** (e.g., `const process (p &Player)`).
*   As a **local variable alias** inside a block (e.g., `let ref &Weapon = player.weapon`).

This guarantees that a reference never outlives the owned variable it points to.

> [!NOTE]
> Rule 2 forbids `&T` as a stored array or slice **element type** — `[*]&T`
> and `[]&T` are always rejected, with no exception. This does not affect
> `for` loops: a `for` loop over an array or slice operates on the original
> array, and mutating its element-binding variable inside the loop body
> mutates the original array directly:
>
> ```lucid
> let players [*]Player = loadPlayers()
>
> for _, plr Player in players {
>     plr.health = plr.health - 10   -- mutates players in place
> }
> ```
>
> If `players` was declared `const`, the loop body cannot mutate `plr`, same
> as any other `const` value — `const` is what governs mutability here, not
> the reference rules above. There is no `[*]&T` array involved at any point;
> the loop simply works on the array you already own.

#### Modeling Complex Data Structures (Trees, Graphs, Links)

Because references (`&T`) cannot be stored inside structs, building circular or linked data structures requires alternative approaches:

1. **Indices and Arenas (Recommended DOD Style):** Store integer indices into a flat array/arena instead of memory references.
   ```lucid
   struct Node {
       value int
       next  int    -- index of the next node in an array / arena
   }
```
2. **Raw Pointers (`*T`):** For manual memory management (e.g., building low-level systems or C integrations), use raw pointers. Raw pointers are "sealed conduits" and require explicit `#toRef` conversion to use, signaling unsafe operations. Type conversion syntax (e.g., `*T(val)` or `&T(val)`) is forbidden for raw pointers and references; the `#toRef` and `#toPtr` intrinsics must be used instead to cross the unsafe boundary.
   ```lucid
   struct Node {
       value int
       next  *Node?    -- raw pointer, nullable. Requires manual lifecycle tracking.
   }
```
3. **Smart Pointers (Standard Library):** For safe shared heap state, use standard library reference-counted wrappers like `Shared<T>` and `Weak<T>` (which auto-nulls when the owner is destroyed). These incur a small runtime cost.

> [!CAUTION]
> **Storing `*T` in a struct field or array element does not carry any extra
> compiler protection over a local `*T`** — see **The Sealed Conduit
> Model** below for the full caveat. Storage duration makes the underlying
> risk worse, not neutral: a local raw pointer's exposure is bounded by the
> function call that holds it, but a stored one persists for as long as the
> struct or array does, with no lifetime tracking at all. A struct holding a
> `*T` field also copies as a **pointer copy**, not a deep clone (see the
> Owned Types table) — two "independent" deep-copied structs can silently
> alias the same pointee through their raw-pointer fields.
>
> **A stored `*func_type` deserves extra caution beyond a stored data
> pointer.** Dereferencing a dangling `*uint8` corrupts a read; *calling*
> through a dangling or aliased `*func_type` transfers control flow to
> whatever now occupies that address — the same failure class as a C
> vtable-smash, not a data bug. Prefer an ordinary Lucid function value
> (named function or closure, see **Function Values and Closures** below) for
> any struct or array field meant to hold callable behavior — those are
> compiler-tracked, not sealed conduits, and have none of this risk. Reserve
> `*func_type` storage strictly for genuine FFI/C-interop callback slots.

### Function Values and Closures

Named functions are plain function pointers — no captured state. Closures (partial applications, anonymous functions capturing variables) hold a heap-allocated environment. Assigning a closure copies the reference to that environment.

---


## The Sealed Conduit Model (Raw Pointers)

Raw pointers (`*T`) are sealed conduits — carry them, pass them to foreign functions, check for nil, but never dereference directly.

### Pointer Nullability Semantics (`*T` vs `*T?`)

`?` on a raw pointer always binds to the **whole pointer type**, not to the element type:

| Type  | Meaning                                                                   | C equivalent                  |
| ----- | ------------------------------------------------------------------------- | ----------------------------- |
| `*T`  | Non-nullable — the pointer address is always valid (programmer assertion) | `T* __attribute__((nonnull))` |
| `*T?` | Nullable — `(*T)?` — the pointer itself may be nil                        | `T*` that may be `NULL`       |

```lucid
const p *Node  = getNode();    -- programmer asserts: address is always valid
const q *Node? = findNode();   -- pointer itself may be nil; nil-check required before use
```

> [!NOTE]
> Unannotated `@[foreign("C")]` pointer returns default to `*T` (non-nullable). Use `*T?` only when you know the C function may return `NULL`. The foreign declaration on the Lucid side is the sole nullability contract — the compiler does not parse C headers.

> [!CAUTION]
> **`*T` is a programmer-level contract, not a compiler-verified proof.**
>
> The type system guarantees that a non-nullable `*T` will never be *statically assigned* `nil`. It does **not** and **cannot** guarantee that the pointed-to memory remains valid at runtime. External code — foreign calls (e.g. `free`), manual pointer arithmetic via `#ptrOffset`, or aliased ownership — can release or corrupt that memory *after* the pointer was set, producing a **dangling pointer**: non-nil in value, invalid in content.
>
> A nil check (`== nil`, `!= nil`) guards against a null address. It does **not** detect a dangling pointer.
>
> **Responsibilities when using non-nullable `*T`:**
> - You assert that the pointer's target is valid for the duration you hold the pointer.
> - You own or have a clear understanding of the pointed-to memory's lifetime.
> - No other owner will free that memory while your `*T` is live.
> - If the `*T` is stored in a struct field or array element rather than
>   held locally, this responsibility extends for as long as that struct or
>   array exists — see **Modeling Complex Data Structures** above, which
>   also covers the added risk of a stored `*func_type`.
>
> **Preferred mitigation — wrap `*T` in a cleanup struct:**
> ```lucid
> struct OwnedBuffer {
>     ptr  *uint8    -- non-nullable: asserts validity at construction
>     size uint64
> }
>
> const disposeBuffer (buf &OwnedBuffer) = {
>     freeBuffer(buf.ptr, buf.size);    -- lifetime ends here, predictably
> }
>```
> For shared ownership with automatic invalidation, use the standard library's `Shared<T>` and `Weak<T>` instead.

### Allowed Operations

1. Store in a variable, struct field, or parameter
2. Pass to a `@[foreign("C")]` function
3. Nil check (`== nil`, `!= nil`)
4. Pass to pointer intrinsics (`#toRef`, `#ptrOffset`, etc.)
5. Print the address for debugging

### Forbidden Operations (Error)

- Dereferencing: `*ptr`
- Field access: `ptr.field`
- Indexing: `ptr[i]`
- Arithmetic: `ptr + 4` — use `#ptrOffset` instead
- Assignment: `*ptr = value`
- Type casting/conversion: e.g., `*float(x)` or `&float(x)` (must use `#toRef` or `#toPtr` to cross safety boundaries)

### Boundary Crossing (Intrinsics)

```lucid
#toRef(ptr);         -- *T → &T  (assert validity, cross to safe reference)
#toPtr(ref);         -- &T → *T  (convert back to raw pointer)
#ptrOffset(ptr, n);  -- pointer arithmetic, returns new *T
#ptrDiff(p1, p2);    -- distance between two pointers as int64
```

```lucid
@[foreign("C")]
const malloc (size uint64) -> *uint8? = {}

const buf *uint8? = malloc(1024);
if buf == nil { return 1 }

let ref &uint8 = #toRef(buf);    -- cross the boundary
ref = 0xFF;    -- work with it safely

const next *uint8? = #ptrOffset(buf, 1);    -- pointer arithmetic
```

### Reading Values Through a Pointer (C → Lucid)

When a C binary exposes data at a known address (e.g. a hardware register, a shared memory region, or a C struct field), the idiomatic Lucid pattern is to declare a `@[foreign("C")]` function that accepts a raw pointer and returns the value by copy — **never by reference**.

> [!IMPORTANT]
> Foreign functions **must not** return `&T`. The Downward Flow Rule forbids reference returns from all functions, including foreign ones — returning a `&T` from C would produce a reference with no Lucid-owned backing variable, which is undefined behaviour. The compiler rejects any foreign declaration with `-> &T` as a return type.
>
> The correct pattern is to return an **owned value** (a primitive, struct, or raw pointer). The caller then uses `#toRef` to enter the safe reference world if needed.

```c
// getValue.c — C side
// Takes a pointer, reads the int at that address, returns it by value.
int getValue(int* address) {
    return *address;
}
```

```lucid
-- main.luc — Lucid side
@[foreign("C")]
const getValue (address *int) -> int = {}    -- returns owned int, never &int

const addr *int = getAddressFromSomewhere();
const n    int  = getValue(addr);    -- safe: int is owned, fully copied
```

If the value at the address is large (a struct), return a raw pointer from C and cross the boundary with `#toRef` on the Lucid side:

```c
// C side — returns a pointer to the struct, caller must not free it
Player* getPlayer(PlayerStore* store, int id) {
    return &store->players[id];
}
```

```lucid
-- Lucid side
@[foreign("C")]
const getPlayer (store *PlayerStore, id int) -> *Player? = {}    -- nullable: id may not exist

const p *Player? = getPlayer(store, 42);
if p == nil { return }

const ref   &Player = #toRef(p);    -- assert validity, enter safe world
const score int     = ref.score;    -- read fields safely through the reference
```

### How C Communicates Nullable Returns to Lucid

C has no built-in nullable type. The foreign declaration in Lucid is the **sole nullability contract** — the Lucid does not parse C headers. The programmer declares the expected nullability and owns the promise.

---

## Type Conversion

There is no `from` declaration in Lucid. All conversions are explicit function
calls. Conversion functions are plain functions by convention:

```lucid
-- naming convention: targetFromSource or targetOf
const intFromString (s string) -> int! = { ... }
const stringFromInt (n int)    -> string = { ... }
const floatFromInt  (n int)    -> float  = { ... }

-- use
const parsed int! = intFromString("42");
const n int = parsed ?? 0;
const s string = stringFromInt(n);
```

### String Interpolation

`\(expr)` embeds an expression directly inside a string literal. Each
embedded expression is type-checked at the point it appears, exactly like
any other expression — there is no separate placeholder syntax (`%s`, `%d`,
and similar) to parse or validate:

```ebnf
interpolation = '\(' expr ')'
```

```lucid
const name string = "alice";
const age  int    = 30;

const greeting string = "hello \(name), you are \(stringFromInt(age)) years old";
-- greeting = "hello alice, you are 30 years old"
```

**`expr` must be of type `string`.** Lucid does not silently stringify a
value of another type — `expr` can be any expression (a variable, a function
call, a whole sub-expression), but whatever it evaluates to must already be
`string`. If the value isn't naturally a `string`, the conversion call is
written inside the parentheses, in the same place it would be written
anywhere else, fully visible and fully type-checked:

```lucid
const n int = 42;

const a string = "value: \(stringFromInt(n))";    -- OK: stringFromInt(n) is string
const b string = "value: \(n)";    -- ERROR: n is int, not string
```

This is the same conversion convention as the rest of this section —
`stringFromInt`, `stringFromFloat`, and so on are ordinary functions, called
the same way whether inside a literal or outside one. Interpolation does not
introduce a second conversion mechanism; it only changes *where* the call can
be written.

**Why `%s`/`%d`-style placeholders are rejected:** a placeholder format
string defers the type relationship between each placeholder and its
corresponding argument to a separate parsing pass over the string's
contents, decoupled from where the argument is actually written. That
relationship is then either checked at runtime (a class of bug this grammar
has consistently avoided — see **Variable Declaration**'s no-inference rule
for the same reasoning applied elsewhere) or requires the compiler to
specially parse string-literal contents as if they were a second grammar,
which both `\(expr)` interpolation and the rest of this compiler avoid:
every expression embedded in a literal is checked exactly where it's
written, by the same type checker that checks everything else.

**Interpolation is forbidden in raw strings** (`"""..."""`) — a raw string's
entire purpose is literal, unprocessed content, so `\(` inside one is just
two literal characters, not the start of an interpolation:

```lucid
const literal string = """value: \(n)""";    -- literally "value: \(n)",
                                                      -- not interpolated
```

**A raw string may span multiple lines in the source.** Unlike `"..."`,
which forbids a literal newline and requires the `\n` escape instead, a raw
string's content is taken verbatim — including every newline and the
indentation on each line — between the opening and closing `"""`:

```lucid
const query string = """;
SELECT id, name
FROM users
WHERE active = true;
"""
-- query contains the literal newlines and leading/trailing whitespace shown
-- above; nothing is stripped or reformatted
```

A single or double quote character inside a triple-quote raw string is
perfectly legal — only three consecutive quotes close it:

```lucid
const msg string = """she said "hello" to me""";    -- OK
const sql string = """WHERE name = 'alice'""";    -- OK
```

> [!NOTE]
> If a raw string literal is written indented — inside a function body, for
> example — that leading whitespace on every line after the first is part of
> the string's content too, since nothing is stripped. Write multi-line raw
> strings starting at column 0, or accept the indentation as literal content,
> rather than expecting the source code's own indentation to be trimmed
> automatically.

A variadic, type-safe alternative for joining already-stringified pieces
also exists, useful when the pieces come from a loop or are otherwise not
known until runtime — see **Variadic Parameters**:

```lucid
const join (parts ...string) -> string = {
    let result string = "";
    for _, p string in parts { result = result + p }
    return result;
}

join("a", "b", "c");    -- "abc"
```

---

## Type Manipulation

There is no `impl` declaration in Lucid. All manipulation on types is expressed as
plain functions that take the type as an explicit parameter:

```lucid
struct Vector2 {
    x float
    y float
}

-- all "methods" are plain functions
const vector2Add  (a Vector2)(b Vector2) -> Vector2 = {
    return Vector2 { x = a.x + b.x, y = a.y + b.y }
}

const vector2Scale (v Vector2)(s float) -> Vector2 = {
    return Vector2 { x = v.x * s, y = v.y * s }
}

const vector2Length (v Vector2) -> float = {
    return sqrt(v.x * v.x + v.y * v.y);
}

-- usage
const a Vector2 = Vector2 { x = 1.0, y = 0.0 }
const b Vector2 = Vector2 { x = 0.0, y = 1.0 }
const c Vector2 = vector2Add(a)(b);
const len float = vector2Length(c);
```

---

## Compiler Directives: Attributes `@` and Intrinsics `#`

Attributes `@[]` and intrinsics `#` provide instructions to the compiler or execute operations.

### 1. Attributes `@[]`

Attributes precede the declaration they annotate and use a bracket-list syntax so multiple items share one delimiter pair.

```ebnf
attribute_list  = '@[' attr_item { ',' attr_item } ']'

attr_item       = IDENTIFIER [ '(' attr_args ')' ]      (* built-in attribute, fixed set *)
attr_args       = attr_arg { ',' attr_arg }
attr_arg        = STRING_LIT | INT_LIT | FLOAT_LIT | BOOL_LIT | IDENTIFIER
```

#### Built-in Attributes

| Attribute              | Valid on                  | Meaning                                                  |
| ---------------------- | ------------------------- | -------------------------------------------------------- |
| `@[export]`            | any top-level declaration | Visible outside this module                              |
| `@[foreign("abi")]`    | function declaration      | Implemented in a foreign compiler; ABI string e.g. `"C"` |
| `@[link("name")]`      | module or declaration     | Link against this native library                         |
| `@[deprecated("msg")]` | any declaration           | Compiler warning at use sites                            |
| `@[inline]`            | function declaration      | Hint to inline at call sites                             |
| `@[noinline]`          | function declaration      | Prevent inlining                                         |

**Rules:**
- `@[foreign]` requires the function body to be empty `{}` — the implementation
  is resolved at link time (compiler) or via dynamic loading (interpreter).
- Attributes are a **fixed, closed set** — there is no user-defined or
  namespaced attribute form.

### 2. Intrinsics `#`

Intrinsics are direct calls into the compiler's backend — a layer shared
between the interpreter and the compiler. They exist because some operations cannot
be expressed as ordinary Lucid functions: querying a type's memory layout, emitting
a specific hardware instruction, performing atomic operations, or managing memory
all require the compiler itself to handle the call.

Unlike a standard library function, an intrinsic has no Lucid body. The compiler 
resolves it entirely — the interpreter executes a built-in implementation,
the compiler emits the corresponding machine instruction or computation directly.
The result is zero-overhead access to hardware capabilities without leaving the
language.

This gives Lucid a clean two-layer model:

- **Lucid code** — safe, expressive, high-level logic. The compiler
  handles memory safety, type checking, and abstractions.
- **Intrinsics** — direct hardware and memory access, zero overhead. You opted
  in explicitly with `#`.

> [!NOTE]
> The `#` prefix is intentional: it signals to the reader that this call steps
> outside what ordinary Lucid code can do. If you see `#`, the compiler
> is doing something on your behalf that you could not write yourself.

```ebnf
intrinsic_call     = '#' IDENTIFIER '(' [ intrinsic_arg_list ] ')'
intrinsic_arg_list = intrinsic_arg { ',' intrinsic_arg }
intrinsic_arg      = expr | type
```

---

#### Type & Value Inspection

Available everywhere — these are read-only observations and carry no safety
concern. They do not manipulate memory, dereference pointers, or escape any
safety boundary.

| Intrinsic     | Returns  | Notes                                                                                                                                                                          |
| ------------- | -------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `#tostr(x)`   | `string` | Human-readable value. Primitives and enums by value; structs as `Name{ field: value, ... }`; functions as their declared name. Calls the `str` field if the struct defines one |
| `#typeof(x)`  | `string` | For any value: its type name. For function types: full signature e.g. `(int, string) -> bool`                                                                                  |
| `#nameof(x)`  | `string` | The declared name of `x` at the call site — variable name, function name, or field name. Resolved entirely at compile time                                                     |
| `#ptrstr(x)`  | `string` | Memory address of `x` as a hex string e.g. `"0x7ffd91a2"`. Read-only, the address itself is not manipulable                                                                    |
| `#addrof(x)`  | `*T`     | Raw memory address of `x`. The pointer is inert until passed to an intrinsic that acts on it                                                                                   |
| `#sizeof(T)`  | `uint64` | Byte size of type `T` — compile-time constant                                                                                                                                  |
| `#alignof(T)` | `uint64` | Alignment requirement of `T` — compile-time constant                                                                                                                           |

```lucid
-- Generic logger: works on any type T
const Log<T> (prefix string, values ...T) = {
    for v in values {
        io:printl(prefix ++ ": " ++ #tostr(v));
    }
}

-- Inspecting a function
const add (a int, b int) -> int = a + b;

io:printl(#nameof(add));    -- "add"
io:printl(#typeof(add));    -- "(int, int) -> int"
io:printl(#ptrstr(add));    -- "0x7ffd91a2"

-- Struct with custom str field — #tostr calls it automatically
struct Point = {
    x float
    y float
    const str () -> string = { return "(" ++ #tostr(x) ++ ", " ++ #tostr(y) ++ ")" }
}

const p Point = Point{ x: 1.5, y: 3.0 }
io:printl(#tostr(p));    -- "(1.5, 3.0)"
io:printl(#typeof(p));    -- "Point"
io:printl(#nameof(p));    -- "p"
```

> [!NOTE]
> `#nameof` reads the source name at the call site, never the runtime value.
> `#addrof` returns a `*T` — the pointer is inert on its own. To act on it you
> must pass it to a pointer intrinsic, which is where the safety boundary sits.

---

#### String Operations

Strings in Lucid are immutable UTF-8 sequences managed by the compiler.
These intrinsics expose low-level string operations the standard library builds on.

| Intrinsic                 | Args                         | Returns  | Notes                                                                  |
| ------------------------- | ---------------------------- | -------- | ---------------------------------------------------------------------- |
| `#str_len(s)`             | `string`                     | `uint64` | Byte length of the string (not codepoint count)                        |
| `#str_ptr(s)`             | `string`                     | `*uint8` | Pointer to the raw UTF-8 bytes — read-only                             |
| `#str_from_ptr(ptr, len)` | `*uint8`, `uint64`           | `string` | Construct a string from raw bytes; compiler copies and validates UTF-8 |
| `#str_concat(a, b)`       | `string`, `string`           | `string` | Concatenate two strings; compiler manages the allocation               |
| `#str_slice(s, from, to)` | `string`, `uint64`, `uint64` | `string` | Byte-range slice; compiler validates bounds and UTF-8 boundaries       |
| `#str_eq(a, b)`           | `string`, `string`           | `bool`   | Byte-exact equality                                                    |
| `#str_byte_at(s, i)`      | `string`, `uint64`           | `uint8`  | Raw byte at position i                                                 |

```lucid
const greet string = "Hello, world!";

io:printl(#tostr(#str_len(greet)));    -- "13"
io:printl(#tostr(#str_byte_at(greet, 0)));    -- "72"  (ASCII 'H')

-- Build a string from a raw byte buffer received via foreign function
@[foreign("C")] const get_buf (out *uint8, len *uint64) = {}

let buf  *uint8 = #alloc(256);
let size uint64 = 0;
get_buf(buf, #addrof(size));
const result string = #str_from_ptr(buf, size);
#free(buf);
```

> [!NOTE]
> The `++` operator in Lucid desugars to `#str_concat`. Prefer `++` in ordinary
> code; use `#str_concat` only when building strings in generic or low-level
> contexts where the operator is not available.

---

#### Memory Management

**Lucid manages all memory automatically for ordinary code.** The user never
calls `#alloc`, `#free`, or any arena intrinsic when writing pure Lucid. Memory
intrinsics exist for one purpose: **bridging into C and other foreign code** that
operates outside Lucid's control.

---

**Automatic — Scope Arena**

Every block has its own scope arena. The compiler pushes a new arena
on entry and pops it on exit, releasing everything inside. Every local value —
non-nullable, nullable, fallible, dynamic array, struct — lives in this arena.
No annotation, no manual free, no lifetime management needed.

```
main() entered           → push scope arena
  if block entered       → push scope arena
    let x int    = 10   → allocated in if-block arena
    let y int?   = nil  → tagged slot in if-block arena
    let z [*]int = []   → dynamic array in if-block arena
  if block exited        → pop arena: x, y, z all released
main() returned          → pop scope arena
```

`nil` and `err` are semantic signals — they change the tag on a slot, not
the memory. The scope arena reclaims the slot at scope exit regardless.

---

**Foreign Interop — Explicit Heap and Arena**

When C or another foreign compiler allocates memory, Lucid has no knowledge of
it. That memory follows C's rules and must be freed the C way. Lucid provides
two tools for working with foreign or explicitly managed memory:

*Lucid-tracked heap* — `#alloc` / `#free`. The compiler knows about
this allocation and catches double-free and null-free. Use this when you need a
raw pointer that Lucid allocates but passes into foreign code:

```lucid
@[foreign("C")] const c_process (buf *uint8, len uint64) = {}

const buf *uint8 = #alloc(uint8, 1024);    -- Lucid allocates, Lucid tracks
#memset(buf, 0, 1024);
c_process(buf, 1024);    -- C reads it
#free(buf);    -- Lucid frees, double-free caught
```

*C-owned memory* — when C allocates and returns a pointer, Lucid cannot track
it. You must free it using the matching C function:

```lucid
@[foreign("C")] const c_malloc (size uint64) -> *uint8 = {}
@[foreign("C")] const c_free   (ptr  *uint8)           = {}

const buf *uint8 = c_malloc(1024);    -- C's memory, Lucid has no knowledge of it
-- ... work with buf via intrinsics ...
c_free(buf);    -- must use C's free, not #free
```

*Named arena* — for bulk allocation patterns where you want to free everything
at once. Useful when building data structures that are handed to foreign code or
when you need predictable allocation layout. The arena is described by an
`ArenaDescriptor` — a plain POD struct that both Lucid and C can read, giving
both sides unambiguous knowledge of the arena's boundaries without any
per-pointer tagging:

```lucid
-- ArenaDescriptor is a built-in POD struct — identical layout on Lucid and C sides
-- Fields are read-only after creation; only the intrinsics may mutate them
const ArenaDescriptor = struct {
    base *uint8   -- start address of the arena region (never changes after create)
    size uint64   -- total byte capacity (never changes after create)
}

-- usage
let arena ArenaDescriptor = #arena_create(4096)
const nodes *Node  = #arena_alloc(arena, Node, 128)
const edges *Edge  = #arena_alloc(arena, Edge, 256)
-- ... build a graph, pass descriptor + pointers to foreign code ...
#arena_free(arena)    -- releases everything at once, no per-slot free needed
```

`ArenaDescriptor` carries only `base` and `size` — the two values needed to
answer the ownership question "did this pointer come from this arena?" The
allocation cursor (`used`) is tracked internally by the compiler and
is not exposed in the descriptor. C never needs to know the cursor — it only
needs the boundaries.

---

**Ownership at a Glance**

| Memory origin                   | Lucid tracks it?      | How it is freed              | C can verify ownership?              |
| ------------------------------- | --------------------- | ---------------------------- | ------------------------------------ |
| Any local value, `[*]T`, struct | Yes — fully automatic | Scope exit                   | N/A — stack memory, never share      |
| `#alloc`                        | Yes — compiler        | `#free` — double-free caught | No — pass size explicitly (Rule 3)   |
| `#arena_alloc`                  | Yes — compiler        | `#arena_free`                | Yes — via `ArenaDescriptor` (Rule 4) |
| C `malloc` / foreign library    | No                    | Matching C free function     | N/A — C owns it entirely             |

| Intrinsic                   | Args                              | Returns           | Notes                                  |
| --------------------------- | --------------------------------- | ----------------- | -------------------------------------- |
| `#alloc(T, count)`          | type, `uint64`                    | `*T`              | Lucid-tracked heap allocation          |
| `#free(ptr)`                | `*T`                              | —                 | Rejects double-free and null-free      |
| `#arena_create(size)`       | `uint64`                          | `ArenaDescriptor` | Create a named arena                   |
| `#arena_alloc(arena, T, n)` | `ArenaDescriptor`, type, `uint64` | `*T`              | Allocate from arena                    |
| `#arena_reset(arena)`       | `ArenaDescriptor`                 | —                 | Release contents; arena remains usable |
| `#arena_free(arena)`        | `ArenaDescriptor`                 | —                 | Destroy arena and all contents         |

---

#### Floating-Point Math

Maps directly to hardware floating-point instructions (e.g. `FSQRT`, `FMADD` on
modern ISAs). Faster and more precise than a software implementation.

| Intrinsic         | Args         | Returns | Notes                                 |
| ----------------- | ------------ | ------- | ------------------------------------- |
| `#sqrt(x)`        | float/double | same    | Hardware square root                  |
| `#floor(x)`       | float/double | same    | Round toward −∞                       |
| `#ceil(x)`        | float/double | same    | Round toward +∞                       |
| `#round(x)`       | float/double | same    | Round to nearest, half away from zero |
| `#abs(x)`         | numeric      | same    | Absolute value                        |
| `#pow(base, exp)` | float/double | same    | Exponentiation                        |
| `#fma(a, b, c)`   | float/double | same    | Fused multiply-add: `(a*b)+c`         |
| `#min(a, b)`      | same type    | same    | Minimum                               |
| `#max(a, b)`      | same type    | same    | Maximum                               |

```lucid
const hyp     float = #sqrt(x*x + y*y);
const rounded float = #round(value);
const clamped float = #max(0.0, #min(1.0, value));
```

---

#### Bit Manipulation (Integer Types Only)

Maps to single CPU instructions (`BSF`, `BSR`, `POPCNT`, `BSWAP` on x86-64).
Essential for low-level data processing, compression, and protocol parsing.

| Intrinsic      | Args    | Returns | Notes                           |
| -------------- | ------- | ------- | ------------------------------- |
| `#clz(x)`      | integer | same    | Count leading zero bits         |
| `#ctz(x)`      | integer | same    | Count trailing zero bits        |
| `#popcount(x)` | integer | same    | Count set (1) bits              |
| `#bswap(x)`    | integer | same    | Reverse byte order (endianness) |

```lucid
const leading  uint32 = #clz(flags);
const trailing uint32 = #ctz(flags);
const bits     uint32 = #popcount(mask);
const swapped  uint32 = #bswap(networkOrder);
```

---

#### SIMD / Vector

Operate on fixed-width vector registers (e.g. SSE/AVX on x86-64, NEON on ARM).
Write scalar Lucid logic for clarity; drop to SIMD explicitly in hot loops for
throughput. The type `vec<T, N>` represents an N-wide vector of element type `T`.

| Intrinsic                   | Args                 | Returns    | Notes                              |
| --------------------------- | -------------------- | ---------- | ---------------------------------- |
| `#simd_load(ptr)`           | `*T`                 | `vec<T,N>` | Load N elements from memory        |
| `#simd_store(ptr, v)`       | `*T`, `vec<T,N>`     | —          | Store N elements to memory         |
| `#simd_add(a, b)`           | `vec<T,N>` × 2       | `vec<T,N>` | Lane-wise addition                 |
| `#simd_sub(a, b)`           | `vec<T,N>` × 2       | `vec<T,N>` | Lane-wise subtraction              |
| `#simd_mul(a, b)`           | `vec<T,N>` × 2       | `vec<T,N>` | Lane-wise multiplication           |
| `#simd_div(a, b)`           | `vec<T,N>` × 2       | `vec<T,N>` | Lane-wise division                 |
| `#simd_fma(a, b, c)`        | `vec<T,N>` × 3       | `vec<T,N>` | Lane-wise fused multiply-add       |
| `#simd_min(a, b)`           | `vec<T,N>` × 2       | `vec<T,N>` | Lane-wise minimum                  |
| `#simd_max(a, b)`           | `vec<T,N>` × 2       | `vec<T,N>` | Lane-wise maximum                  |
| `#simd_splat(T, N, scalar)` | type, int, `T`       | `vec<T,N>` | Broadcast scalar to all lanes      |
| `#simd_extract(v, i)`       | `vec<T,N>`, int      | `T`        | Extract lane i                     |
| `#simd_insert(v, i, x)`     | `vec<T,N>`, int, `T` | `vec<T,N>` | Return v with lane i replaced by x |

```lucid
-- Sum an array of floats using 4-wide SIMD
const sumFloats (data *float32, len uint64) -> float32 = {
    let acc vec<float32, 4> = #simd_splat(float32, 4, 0.0);
    let i   uint64          = 0;

    while i + 4 <= len {
        const chunk vec<float32, 4> = #simd_load(#ptrOffset(data, i));
        acc = #simd_add(acc, chunk);
        i = i + 4;
    }

    return #simd_extract(acc, 0) + #simd_extract(acc, 1);
         + #simd_extract(acc, 2) + #simd_extract(acc, 3);
}
```

---

#### Atomics

Lock-free operations on shared memory. Required for concurrent data structures
and synchronization primitives. All atomic intrinsics take an explicit memory
ordering argument.

| Ordering  | Meaning                                                      |
| --------- | ------------------------------------------------------------ |
| `relaxed` | No synchronization — only atomicity of the operation itself  |
| `acquire` | Subsequent reads/writes in this thread see prior releases    |
| `release` | Prior reads/writes in this thread are visible before this op |
| `acq_rel` | Both acquire and release — for read-modify-write operations  |
| `seq_cst` | Total sequential consistency across all threads              |

| Intrinsic                         | Args                     | Returns | Notes                                     |
| --------------------------------- | ------------------------ | ------- | ----------------------------------------- |
| `#atomic_load(ptr, ord)`          | `*T`, ordering           | `T`     | Atomic read                               |
| `#atomic_store(ptr, val, ord)`    | `*T`, `T`, ordering      | —       | Atomic write                              |
| `#atomic_add(ptr, val, ord)`      | `*T`, `T`, ordering      | `T`     | Fetch-and-add; returns previous value     |
| `#atomic_sub(ptr, val, ord)`      | `*T`, `T`, ordering      | `T`     | Fetch-and-sub; returns previous value     |
| `#atomic_and(ptr, val, ord)`      | `*T`, `T`, ordering      | `T`     | Fetch-and-and; returns previous value     |
| `#atomic_or(ptr, val, ord)`       | `*T`, `T`, ordering      | `T`     | Fetch-and-or; returns previous value      |
| `#atomic_xor(ptr, val, ord)`      | `*T`, `T`, ordering      | `T`     | Fetch-and-xor; returns previous value     |
| `#atomic_cas(ptr, exp, val, ord)` | `*T`, `T`, `T`, ordering | `bool`  | Compare-and-swap; returns true if swapped |

```lucid
-- Lock-free reference counter
const retain (refcount *uint32) = {
    #atomic_add(refcount, 1, relaxed);
}

const release (refcount *uint32) -> bool = {
    const prev uint32 = #atomic_sub(refcount, 1, acq_rel);
    return prev == 1;    -- true means count hit zero
}

-- CAS spin loop
const claimSlot (flag *uint32) = {
    while not #atomic_cas(flag, 0, 1, acq_rel) {
        #pause();
    }
}
```

---

#### CPU Hints

Hints to the CPU or memory subsystem. Do not affect correctness — they only
influence performance. The compiler may ignore them on targets where
the hint has no equivalent instruction.

| Intrinsic          | Args     | Returns | Notes                                              |
| ------------------ | -------- | ------- | -------------------------------------------------- |
| `#prefetch(ptr)`   | `*T`     | —       | Hint CPU to load cache line into L1                |
| `#prefetch_w(ptr)` | `*T`     | —       | Prefetch for write                                 |
| `#fence(ord)`      | ordering | —       | Explicit memory barrier                            |
| `#pause()`         | —        | —       | Spin-wait hint; reduces power in busy-wait loops   |
| `#likely(expr)`    | `bool`   | `bool`  | Hint that expr is usually true (branch prediction) |
| `#unlikely(expr)`  | `bool`   | `bool`  | Hint that expr is usually false                    |

```lucid
for i uint64 in 0..len {
    #prefetch(#ptrOffset(data, i + 8));    -- prefetch ahead
    process(data[i]);
}

if #likely(cache_hit) {
    return cached;
} else {
    return slowPath();
}
```

---

#### Raw Memory Operations

Bulk memory operations on raw pointers. Used primarily in interop with foreign
functions or when building low-level data structures.

| Intrinsic                 | Args                    | Returns | Notes                                          |
| ------------------------- | ----------------------- | ------- | ---------------------------------------------- |
| `#memcpy(dst, src, len)`  | `*T`, `*T`, `uint64`    | —       | Copy `len` bytes — regions must not overlap    |
| `#memmove(dst, src, len)` | `*T`, `*T`, `uint64`    | —       | Copy `len` bytes — handles overlapping regions |
| `#memset(dst, val, len)`  | `*T`, `uint8`, `uint64` | —       | Fill `len` bytes with `val`                    |

```lucid
const dst *uint8 = #alloc(uint8, #sizeof(Buffer));
#memcpy(dst, src, #sizeof(Buffer));
#memset(dst, 0, #sizeof(Buffer));
#free(dst);
```

---

#### Pointer Operations

Cross the safe/pointer boundary or perform pointer arithmetic. All pointer
operations go through intrinsics — there is no implicit pointer arithmetic or
automatic dereference.

| Intrinsic            | Args          | Returns | Notes                                           |
| -------------------- | ------------- | ------- | ----------------------------------------------- |
| `#toRef(ptr)`        | `*T`          | `&T`    | Assert non-null and convert to a safe reference |
| `#toPtr(ref)`        | `&T`          | `*T`    | Convert a safe reference to a raw pointer       |
| `#ptrOffset(ptr, n)` | `*T`, `int64` | `*T`    | Advance pointer by n elements                   |
| `#ptrDiff(p1, p2)`   | `*T`, `*T`    | `int64` | Distance between two pointers in elements       |

```lucid
const buf  *uint8 = #alloc(uint8, 1024);
const ref  &uint8 = #toRef(buf);    -- assert non-null, enter safe world
ref = 0xFF;

const next     *uint8 = #ptrOffset(buf, 1);
const distance int64  = #ptrDiff(next, buf);    -- 1
#free(buf);
```

---

#### Bit Reinterpretation

| Intrinsic        | Args        | Returns | Notes                                                                       |
| ---------------- | ----------- | ------- | --------------------------------------------------------------------------- |
| `#bitcast(T, x)` | type, value | `T`     | Reinterpret the bits of `x` as type `T`. Both types must have the same size |

```lucid
const bits uint32  = 0x3F800000;
const f    float32 = #bitcast(float32, bits);    -- 1.0
```

---

## Foreign Function Interface

Lucid's execution model is a natural match for C: functions are plain symbols,
calls are predictable, and all behaviour is visible at the call site. The only
supported ABI string is `"C"` — there is no `@[foreign("C++")]`.

C++ introduces mechanisms that violate this predictability:

- **Constructors and destructors** — code runs invisibly on object creation and
  destruction. Lucid has no scope-exit hooks; a C++ object whose destructor
  Lucid fails to call silently corrupts state.
- **Exceptions** — a `throw` unwinds the call stack invisibly. Lucid's runtime
  has no C++ stack unwinding tables; an uncaught exception crossing the boundary
  is undefined behaviour.
- **Name mangling** — C++ encodes the full signature into the symbol name. Lucid
  cannot predict the mangled name without running a C++ compiler. C symbols are
  exactly what you write.
- **Implicit `this` pointer** — C++ member functions take a hidden first argument
  that does not appear in the source signature. Lucid requires every parameter to
  be declared explicitly.
- **Templates** — instantiated at compile time, producing mangled symbols that
  depend on type arguments. There is no stable symbol name to link against.

The solution is a thin `extern "C"` wrapper written in C++ that converts all of
the above into a flat, predictable C surface. Once the wrapper exists, Lucid
sees pure C and `@[foreign("C")]` works normally. The wrapper does not change how
you use the library — it is a translation layer, not a reimplementation.

```
[C++ library] → [extern "C" wrapper] → [@[foreign("C")] declarations] → [Lucid]
```

> [!NOTE]
> **Interpreter Mode:** `@[foreign]` functions are resolved by the LLVM ORC JIT's
> built-in dynamic linker. Libraries named in `@[link(...)]` are loaded via
> `dlopen` / `LoadLibrary` at startup and their symbols are registered with the
> JIT's symbol table. LLVM codegen handles calling conventions — no separate
> marshaling layer is needed. The compiler resolves them via the system linker
> at link time instead.

```ebnf
foreign_decl    = '@[' foreign_attr { ',' link_attr } ']' func_decl
                  (* func_body must be empty '{}' *)

foreign_attr    = 'foreign' '(' '"C"' ')'
                  (* only "C" is a valid ABI string — see C++ Interop below *)

link_attr       = 'link' '(' STRING_LIT { ',' STRING_LIT } ')'
                  (* one or more library names or source file paths *)
```

> [!NOTE]
> `@[link(...)]` accepts one or more comma-separated strings. Each string is
> either a library name (e.g. `"opengl"`, `"m"`) or a source/object file path
> (e.g. `"vendor/lib/file.c"`). Bare names are passed as `-lname` to the linker;
> paths are passed directly. The two forms can be mixed freely.

> [!IMPORTANT]
> Foreign functions **must not** return `&T`. The Downward Flow Rule forbids
> reference returns from all functions including foreign ones. Return an owned
> value or a raw `*T` and use `#toRef` on the Lucid side if needed.

---

### C Interop

```lucid
-- standard C library function
@[foreign("C")]
const malloc (size uint64) -> *uint8? = {}

-- combine foreign + link in one attribute list
@[foreign("C"), link("path/to/file.c")]
const myAdd (a int32, b int32) -> int32 = {}

-- no return value: omit the return type entirely
@[foreign("C"), link("opengl")]
const glClear (mask uint32) = {}

-- nullable return — C function may return NULL
@[foreign("C"), link("mylib")]
const findNode (id int32) -> *Node? = {}

-- multiple link targets: paths and library names can be mixed
@[foreign("C"), link("vendor/math/fast_math.c", "vendor/math/lut.c", "m")]
const fastSin (x float32) -> float32 = {}
```

**Type mapping — C to Lucid:**

| C type          | Lucid type | Notes                                                 |
| --------------- | ---------- | ----------------------------------------------------- |
| `int`           | `int32`    |                                                       |
| `unsigned int`  | `uint32`   |                                                       |
| `long`          | `int64`    | platform-dependent in C; treat as 64-bit              |
| `size_t`        | `uint64`   |                                                       |
| `float`         | `float32`  |                                                       |
| `double`        | `float64`  |                                                       |
| `char *`        | `*uint8`   | not a Lucid `string` — use `#str_from_ptr` to convert |
| `void *`        | `*uint8`   | conventional untyped byte pointer                     |
| `T *`           | `*T`       | raw pointer; nullable if C may return `NULL` → `*T?`  |
| `void` (return) | omitted    | no return type in the Lucid declaration               |

**Exporting Lucid functions to C:**

```lucid
@[export, foreign("C")]
const add (a int32, b int32) -> int32 = {
    return a + b;
}
```

`@[export]` makes the symbol visible to the linker. `@[foreign("C")]` forces the
C ABI on the call boundary. Both attributes are required when exposing a Lucid
function to a C caller.

---

### C++ Interop

C++ cannot be called directly from Lucid. Write a thin `extern "C"` wrapper in
C++ that exposes a flat C surface, then declare that wrapper in Lucid using
`@[foreign("C")]` as normal. The wrapper translates every C++ mechanism into
something Lucid can see:

- C++ class → opaque `*uint8` handle (pass-through only — never dereference on the Lucid side)
- Constructor/destructor → explicit `_create` / `_destroy` functions
- Member functions → free functions with an explicit `self *uint8` first parameter
- C++ exceptions → caught in the wrapper, converted to a nullable return

```cpp
// kernel_wrapper.cpp — thin C surface over a C++ kernel library
#include "kernel.hpp"

extern "C" {
    Kernel* kernel_create(int config) {
        try {
            return new Kernel(config);
        } catch (...) {
            return nullptr;   // exceptions become NULL on the C side
        }
    }

    void kernel_destroy(Kernel* self) {
        delete self;
    }

    int kernel_run(Kernel* self, float* data, int len) {
        try {
            return self->run(data, len);
        } catch (...) {
            return -1;
        }
    }
}
```

```lucid
-- Lucid sees a flat C surface — no C++ anywhere in these declarations
@[foreign("C"), link("kernel_wrapper.cpp", "kernel")]
const kernel_create  (config int32) -> *uint8? = {}

@[foreign("C"), link("kernel_wrapper.cpp", "kernel")]
const kernel_destroy (self *uint8) = {}

@[foreign("C"), link("kernel_wrapper.cpp", "kernel")]
const kernel_run     (self *uint8, data *float32, len int32) -> int32 = {}

-- usage: the C++ object is an opaque handle on the Lucid side
const k *uint8? = kernel_create(42);
if k == nil {
    -- construction failed (exception was caught in wrapper)
}
const result int32 = kernel_run(k, dataPtr, 1024);
kernel_destroy(k);
```

The wrapper scales linearly — one wrapper function per C++ method you need to
expose. It is mechanical enough that it could be generated from C++ headers
automatically. The full power of the C++ library is available; you are just
accessing it through a stable, predictable door.

> [!WARNING]
> The C++ object is an opaque `*uint8` on the Lucid side. Lucid has no knowledge
> of its layout, ownership, or lifetime. `kernel_destroy` must be called exactly
> once, and `k` must not be used after that point. The compiler cannot
> catch these violations.

---

### Memory Safety at the Foreign Boundary

Lucid guarantees memory safety within its own code. That guarantee **ends at the
foreign boundary**. A C or C++ function that receives a Lucid pointer can corrupt
Lucid's memory in ways the compiler cannot detect or prevent:

- **Premature free** — C calls `free(ptr)` on a pointer Lucid still owns,
  producing a dangling pointer inside Lucid's scope arena or heap.
- **Double free** — C frees a `#alloc`-ed pointer; Lucid later calls `#free` on
  the same address. The compiler catches the second `#free`, but the
  window between C's free and Lucid's attempted free is a dangling pointer.
- **Buffer overrun** — C writes beyond the end of its allocation into adjacent
  Lucid memory.

These are not compiler bugs — they are the known cost of crossing into unmanaged
territory. Lucid addresses them through four rules and one pattern:

**Rule 1 — Never pass a scope arena address to foreign code that outlives the call**

A scope arena pointer is only valid for the current scope's lifetime. If C stores
the address and uses it after the call returns, it holds a dangling pointer into
freed memory. Only `#alloc`-ed or `#arena_alloc`-ed pointers may be passed to
foreign functions that need to hold the address past the call:

```lucid
-- WRONG: scope arena address passed to C that stores it
let player Player = Player{ ... };
c_register(#addrof(player));   -- ERROR: player lives on the scope arena;
                                 -- if C holds this past the scope exit it
                                 -- is a dangling pointer

-- CORRECT: heap-allocated, stable address
const player *Player = #alloc(Player, 1);
c_register(player);            -- safe: #alloc lives until #free
```

**Rule 2 — Never mix allocators**

If Lucid allocated it with `#alloc`, only `#free` may release it. If C needs to
own the lifetime, allocate with C's `malloc` via a foreign declaration and free
with C's `free`. Never call C's `free` on a `#alloc`-ed pointer or `#free` on a
`malloc`-ed pointer:

```lucid
-- Lucid owns: allocated and freed by Lucid
const buf *uint8 = #alloc(uint8, 1024);
c_read_into(buf, 1024);   -- C reads/writes but does not free
#free(buf);

-- C owns: allocated and freed by C
@[foreign("C")] const c_malloc (size uint64) -> *uint8? = {}
@[foreign("C")] const c_free   (ptr *uint8) = {}

const cbuf *uint8? = c_malloc(1024);
c_process(cbuf);
c_free(cbuf);             -- freed by C's allocator, not #free
```

**Rule 3 — Always pass buffer sizes explicitly**

Every pointer passed to C that represents a buffer must be accompanied by its
size. C has no way to know the buffer's extent:

```lucid
const buf *uint8 = #alloc(uint8, 1024);
c_fill(buf, 1024);   -- size passed explicitly — C knows its bounds
#free(buf);
```

**Rule 4 — Use `ArenaDescriptor` when sharing arena memory with C**

Passing a bare pointer into arena-allocated memory gives C no way to know where
the arena begins or ends, and no way to verify whether a pointer belongs to it.
Always pass the `ArenaDescriptor` alongside any arena-allocated pointer given to
C. The descriptor gives C unambiguous boundary information — `base` and `size`
are enough to answer "is this pointer mine to free?" with a single range check.
Never pass a raw `*uint8` into arena memory to C without its descriptor:

```lucid
-- WRONG: C receives arena memory but has no boundary information
let arena ArenaDescriptor = #arena_create(65536)
const data *uint8 = #arena_alloc(arena, uint8, 4096)
c_process(data)    -- C has no way to verify ownership or bounds

-- CORRECT: C receives both the data and the descriptor
let arena ArenaDescriptor = #arena_create(65536)
const data *uint8 = #arena_alloc(arena, uint8, 4096)
c_process(data, #addrof(arena))    -- C can verify: is data inside arena?
```

**Pattern — Arena as a C sandbox**

The safest pattern for giving C a region to work in is a named arena backed by
an `ArenaDescriptor`. Lucid allocates slices from the arena and passes both the
slices and the descriptor to C. C receives a complete picture of the arena's
boundaries — it can verify that any pointer it holds falls within the region and
must not call `free()` on arena memory. When C is done, Lucid frees the whole
arena at once.

```lucid
-- Lucid side: create arena and allocate slices from it
let arena ArenaDescriptor = #arena_create(65536)
const nodes *uint8 = #arena_alloc(arena, uint8, 4096)
const edges *uint8 = #arena_alloc(arena, uint8, 8192)

-- pass both the slices and the descriptor to C
-- C receives: the data pointers AND the arena's base + size
-- C can verify ownership: is this pointer inside arena.base .. arena.base + arena.size?
-- C must not call free() on nodes or edges — they are not malloc-ed addresses
c_build_graph(nodes, edges, #addrof(arena))

#arena_free(arena)    -- Lucid frees everything at once when C is done
```

On the C side, the descriptor arrives as a plain POD struct with an identical
layout — no vtable, no padding surprises, no hidden fields:

```c
// C side — ArenaDescriptor layout matches the Lucid definition exactly
typedef struct {
    uint8_t* base;   // start of the arena region
    uint64_t size;   // total byte capacity
} LGE_ArenaDescriptor;

// Ownership check — call this before doing anything risky with a pointer
static int lge_arena_contains(const LGE_ArenaDescriptor* arena, const void* ptr) {
    return (uint8_t*)ptr >= arena->base &&
           (uint8_t*)ptr <  arena->base + arena->size;
}

void c_build_graph(uint8_t* nodes, uint8_t* edges,
                   const LGE_ArenaDescriptor* arena)
{
    // verify before use — cheap range check, no hash lookup
    assert(lge_arena_contains(arena, nodes));
    assert(lge_arena_contains(arena, edges));

    // work with nodes and edges freely — never call free() on them
}
```

> [!IMPORTANT]
> `ArenaDescriptor.base` and `ArenaDescriptor.size` are set once at
> `#arena_create` and never change. They are safe to read from C at any point
> during the arena's lifetime. The allocation cursor is managed internally by
> the compiler and is not exposed in the descriptor — C does not need
> it and must not attempt to track it.

> [!WARNING]
> Passing `#addrof(arena)` to C is only safe when the `ArenaDescriptor` variable
> outlives the C call. Declare arena descriptors at the outermost scope that
> needs them — never as a short-lived local inside a loop that calls C.

---

### Nullability Contract

C has no nullable type. The Lucid declaration is the **sole nullability contract**
— Lucid does not parse C headers. The programmer declares the expected nullability
and owns that promise.

```lucid
-- programmer promises this never returns NULL → declared non-nullable
@[foreign("C")]
const getGlobalState () -> *State = {}

-- programmer knows this may return NULL → declared nullable
@[foreign("C")]
const findUser (id int32) -> *User? = {}
```

Unannotated pointer returns default to non-nullable (`*T`). Use `*T?` only when
you know the foreign function may return `NULL`.

---

## Operator Precedence

Highest to lowest:

| Level | Operators                   | Associativity |
| ----- | --------------------------- | ------------- |
| 8     | `+>` (composition)          | left          |
| 7     | unary `-` `not` `~`         | right         |
| 6     | `*` `/` `%` `**`            | left          |
| 5     | `+` `-`                     | left          |
| 4     | `..` `..<` (range)          | left          |
| 3     | `==` `!=` `<` `<=` `>` `>=` | left          |
| 2     | `and`                       | left          |
| 1     | `or`                        | left          |
| 0     | `\|>` (pipeline)            | left          |

`..`/`..<` bind tighter than comparison so range bounds can be ordinary
arithmetic without parentheses (`0..n - 1`), but looser than `+`/`-`/`*`/`/`
so each bound is fully evaluated before the range is formed.

---

Here's a comprehensive section for Async/Await that fits seamlessly into your Lucid grammar:

---

Here are the two sections rewritten for your unified concurrency model:

---

## Async / Await

`async` and `await` are **statement keywords** for cooperative concurrency. They appear at the call site as statements. A function does not need to declare itself `async` — any function can be called with `async` at the call site.

> [!NOTE]
> **Concurrency vs Parallelism in Lucid:**
>
> | Feature | Model | Implementation | Best For |
> | ------- | ----- | -------------- | -------- |
> | `async`/`await` | Concurrency | Single-threaded event loop | I/O-bound, high-volume operations |
> | `spawn`/`join` | Parallelism | OS threads | CPU-bound work, blocking operations |
>
> `async`/`await` uses **cooperative multitasking** on a single thread. `spawn`/`join` uses **preemptive multitasking** on OS threads. The two features are complementary — they can be mixed freely in the same program.

### `async` — Schedule Concurrent Operations

`async` schedules a function call on the event loop and binds its return value to one or more existing variables. The calling thread continues running immediately — the async operation runs concurrently with the caller, but on the same thread (cooperative multitasking).

```ebnf
async_stmt      = 'async' IDENTIFIER { ',' IDENTIFIER } '=' call_expr
                  (* one variable per return value of call_expr *)
                  (* variables must already be declared (let) *)
                  (* call_expr must return a type that can be awaited *)
```

```lucid
-- single return value
let result string;
async result = fetchData("https://api.example.com");

-- do other work while fetchData runs
let n int = 1 + 2;
for i int in 0..1000 {
    n = n + i;
}

-- later, wait for the result
await result;
io:printl(result);
```

**Multiple return values** follow the same pattern — one variable per returned value, in the same order as the function's return type:

```lucid
const parseInt (s string) -> (int, bool) = { ... }

let value int;
let ok bool;
async value, ok = parseInt("42");

-- ... other work ...

await value, ok;
if ok { io:printl(stringFromInt(value)) }
```

### `await` — Wait for Async Results

`await` blocks the current thread until one or more async operations complete. If multiple variables are awaited in one statement, the thread waits until **all** of them are ready.

```ebnf
await_stmt      = 'await' IDENTIFIER { ',' IDENTIFIER }
                  (* waits for all named variables to be filled *)
                  (* each IDENTIFIER must be a variable bound by async *)
```

```lucid
-- Wait for a single async operation
await result;
io:printl(result);

-- Wait for multiple async operations to complete
let user User;
let profile Profile;
async user = fetchUser(1);
async profile = fetchProfile(1);
await user, profile;
io:printl(user.name + ": " + profile.bio);
```

**If `await` is never called**, the async operation runs until the main thread terminates — at which point all unawaited async operations are also terminated. The variables bound by `async` remain unset if `await` is never reached.

> [!WARNING]
> The compiler **warns** about unawaited async operations when the scope exits:
>
> ```lucid
> const process () -> () = {
>     let result string
>     async result = fetchData(url);
>
>    -- If we exit without awaiting, the async operation is terminated
>    -- WARNING: 'result' was bound by async but never awaited
> }
>```

### Cooperative Multitasking — The Event Loop

Async operations in Lucid are **cooperative**, not preemptive. A task runs until it explicitly yields control at an `await` point. This means:

1. **No race conditions on shared data** — tasks only yield at known points (when awaiting), so data accessed between yield points is thread-safe by construction.
2. **Predictable scheduling** — tasks run to completion unless they yield.
3. **No OS thread overhead** — thousands of async operations can run concurrently on one thread.

```lucid
-- Three async operations sharing data safely
let counter int = 0;

async task1 = {
    -- task1 runs until it hits an await
    counter = counter + 1;    -- safe: no other task runs here
    await someIo();
    counter = counter + 1;    -- still safe: we yielded, but no other task
}    -- can modify counter unless it also yields

async task2 = {
    counter = counter + 2;    -- safe: happens in its own time slice
    await otherIo();
    counter = counter + 2;
}

await task1, task2;
-- counter is predictable: 0 → 1 → 1 → 3 → 3 → 6
-- (order depends on scheduling, but each individual operation is atomic)
```

> [!WARNING]
> Cooperative multitasking does **not** eliminate the need for synchronization when:
> - Data is shared between `async` tasks and **OS threads** (`spawn`/`join`)
> - Data is shared between async tasks that yield inside a **critical section**
>
> In those cases, use standard library synchronization primitives (mutexes, atomics).

### Async Operations in Loops

Scheduling many async operations in a loop is idiomatic and efficient:

```lucid
const urls [*]string = ["https://api1.com", "https://api2.com", "https://api3.com"];

-- Schedule all fetches concurrently
let results [*]string = [];
for _, url string in urls {
    let result string;
    async result = fetchData(url);
    arr:append<string>(results)(result);    -- store the variable reference
}

-- Wait for all to complete
for _, result string in results {
    await result;
}

-- All data is now ready
for _, result string in results {
    io:printl(result);
}
```

### Error Handling with Async/Await

Async operations can return fallible or nullable types. The same narrowing rules apply:

```lucid
let data string!;
async data = riskyFetch(url);

-- Narrow before use
if data == err {
    log("fetch failed");
    return;
}
-- data is string here

const result string = data ?? "fallback";
```

### Combining Async with Spawn

Async (concurrency) and spawn (parallelism) can be mixed freely:

```lucid
-- CPU-bound work in a separate thread
spawn heavyResult = processLargeDataset(data);

-- Many I/O operations on the event loop
let files [*]string;
for _, path string in filePaths {
    let content string;
    async content = readFile(path);
    arr:append<string>(files)(content);
}

-- Wait for all I/O first (fast)
for _, content string in files {
    await content;
}

-- Then wait for the CPU work (slow)
join heavyResult;

io:printl("All work complete: " + stringFromInt(heavyResult));
```

### Await Ordering

`await` only waits for operations scheduled with `async`. You cannot `await` a `spawn` operation:

```lucid
let result string;
spawn result = heavyWork();    -- result is thread-bound

await result;    -- ERROR: result is not an async operation
join result;    -- CORRECT: wait for the thread

let light string;
async light = ioWork();    -- light is event-loop-bound

join light;    -- ERROR: light is not a thread
await light;    -- CORRECT: wait for the async operation
```

### Async and the Visual Graph

```
[@[export] const main (args [_]string)]
    │
    [let result string]
    │
    [async | result | fetchData]──┐
    │                             │ fetchData running on event loop
    ...continue execution         │
    │                             │
    [await | result] ─────────────┘  -- pause until result ready
    │
    [io:printl(result)]
    │
    [end]
```

**Visual distinction from spawn:**

```
[spawn | heavyResult | expensiveWork]────┐  ← OS thread (dashed border)
                                          │
[async | lightResult | ioWork]──┐         │  ← Event loop task (solid border, different color)
                                 │         │
[await | lightResult] ──────────┘         │  ← Wait for event loop
                                 │         │
[join | heavyResult] ─────────────────────┘  ← Wait for OS thread
```

### Performance Guidelines

| Operation Type     | Use     | Count     | Reasoning                                                  |
| ------------------ | ------- | --------- | ---------------------------------------------------------- |
| Network I/O        | `async` | 1000+     | Event loop handles many concurrent connections efficiently |
| File I/O           | `async` | 100+      | Filesystem operations yield at the OS level                |
| CPU-bound          | `spawn` | CPU cores | Requires actual parallelism on OS threads                  |
| Lightweight timers | `async` | Thousands | `sleep` can yield without threads                          |

### Complete Example

```lucid
import std.io as io
import std.http as http

-- Fetch multiple URLs concurrently
const fetchAll (urls [*]string) -> [*]string = {
    let results [*]string = [];

    -- Schedule all fetches
    for _, url string in urls {
        let data string;
        async data = http:get(url);
        arr:append<string>(results)(data);
    }

    -- Wait for all fetches to complete
    let fetched [*]string = [];
    for _, data string in results {
        await data;
        arr:append<string>(fetched)(data);
    }

    return fetched;
}

-- Mixed parallelism and concurrency
@[export] const main () -> int = {
    let urls [*]string = [
        "https://api1.com/users",
        "https://api2.com/products",
        "https://api3.com/orders"
    ]

    -- Concurrent I/O (event loop)
    let userData string;
    let productData string;
    let orderData string;
    async userData = http:get(urls[0]);
    async productData = http:get(urls[1]);
    async orderData = http:get(urls[2]);

    -- Parallel CPU work (OS thread)
    let processed string;
    spawn processed = processUserData(userData);

    -- Wait for I/O first
    await userData, productData, orderData;

    -- Parse results while CPU work continues
    const users [*]User = parseUsers(userData);
    const products [*]Product = parseProducts(productData);
    const orders [*]Order = parseOrders(orderData);

    -- Wait for CPU work to finish
    join processed;

    io:printl("Users: " + stringFromInt(len(users)));
    io:printl("Products: " + stringFromInt(len(products)));
    io:printl("Orders: " + stringFromInt(len(orders)));
    io:printl(processed);

    return 0;
}
```

### Async vs Spawn — Decision Tree

```lucid
-- When to use each:
--
-- Need to wait for a result?
--   │
--   ├─ Yes → Is it blocking I/O?
--   │         ├─ Yes → `async` (concurrent, event loop)
--   │         └─ No → `spawn` (parallel, OS thread)
--   │
--   └─ No → `spawn _ =` (fire and forget)
--
-- Have thousands of operations?
--   │
--   ├─ Yes → `async` (event loop scales to 10,000+)
--   └─ No → `spawn` (OS threads, limited by CPU cores)
--
-- Need to share data with minimal locking?
--   │
--   ├─ Yes → `async` (cooperative = safe yield points)
--   └─ No → `spawn` (preemptive = needs locks)
```

---

## Spawn / Join

`spawn` and `join` are **statement keywords** for thread-based parallelism. `spawn` launches a function call on a separate OS thread. `join` later blocks the calling thread until spawned operations complete.

```ebnf
spawn_stmt      = 'spawn' spawn_binding { ',' spawn_binding } '=' call_expr
                  (* one binding per return value of call_expr *)

spawn_binding   = IDENTIFIER      (* store result for later join *)
                | '_'             (* discard result — fire and forget *)

join_stmt       = 'join' IDENTIFIER { ',' IDENTIFIER }
                  (* waits for all named spawn results to be ready *)
```

### Fire and Forget (`_`)

Use `_` when you don't need the return value. The spawned thread runs independently and is never joined:

```lucid
-- Logging, cleanup, background tasks
spawn _ = logToFile("application started");
spawn _ = garbageCollect();
spawn _ = sendAnalytics();

-- Main thread continues immediately
io:printl("main thread continues while background tasks run");
```

**When the main thread exits**, all unjoined threads are terminated immediately. This is fine for fire-and-forget tasks that are meant to be background work.

### Fire and Join (named variable)

Use a named variable when you need the result. The spawned thread runs in parallel while you continue working:

```lucid
-- Single return value
let result int;
spawn result = computeHeavyData();

-- Do other work while computeHeavyData runs
let n int = 1 + 2;
for i int in 0..1000 {
    n = n + i;
}

-- Block until result is ready
join result;
io:printl("Result: " + stringFromInt(result));
```

**Multiple return values** follow the same pattern — one variable per returned value, in the same order as the function's return type:

```lucid
const parseData (s string) -> (int, bool) = { ... }

let value int;
let ok bool;
spawn value, ok = parseData("42");

-- ... other work ...

join value, ok;
if ok { io:printl(stringFromInt(value)) }
```

### Mixing Discarded and Kept Results

When a function returns multiple values, you can keep some and discard others:

```lucid
const processUser (data string) -> (User, AuditLog, bool) = { ... }

-- Only need the User, discard the rest
let user User;
spawn user, _, _ = processUser(rawData);
join user;

-- Or keep everything
let user User;
let log AuditLog;
let valid bool;
spawn user, log, valid = processUser(rawData);
join user, log, valid;
```

### The Discard Pattern (`_`) vs Named Variables

| Syntax           | Meaning             | Join Required? | Result Available? |
| ---------------- | ------------------- | -------------- | ----------------- |
| `spawn _ = fn()` | Fire and forget     | ❌ No           | ❌ No              |
| `spawn x = fn()` | Fire and join later | ✅ Yes          | ✅ Yes             |

```lucid
-- The discard pattern is explicit about intent
spawn _ = backgroundTask();    -- Clearly: I don't care about the result

-- Named variables signal: I'll need this later
spawn result = heavyWork();    -- Clearly: I'll join this eventually
```

### Compiler Enforcement

The compiler **warns** about named spawns that are never joined:

```lucid
const process () -> int = {
    spawn result = heavyWork();    -- result is never joined
    return 0;
}
-- COMPILER WARNING: spawned result 'result' is never joined
```

To silence the warning, either join the result or explicitly discard it:

```lucid
const process () -> int = {
    -- Option 1: Join before returning
    let result int;
    spawn result = heavyWork();
    join result;
    return result;

    -- Option 2: Discard intentionally
    spawn _ = heavyWork();
    return 0;
}
```

### Shared State

Every variable and function declared before the `spawn` call is shared between threads. This is how threads communicate:

```lucid
let sharedCounter int = 0;

spawn _ = {
    -- This runs on a separate thread
    sharedCounter = sharedCounter + 1;
}

let result int;
spawn result = {
    -- Another thread, also can access sharedCounter
    sharedCounter = sharedCounter + 1;
    return sharedCounter;
}

join result;
```

> [!WARNING]
> Concurrent writes to shared variables are not automatically synchronized.
> Use shared state carefully — design the shared struct so each thread owns
> distinct fields, or add explicit synchronization through the standard
> library.

### Nesting

A spawned thread can itself launch further `spawn` calls:

```lucid
const processData () -> int = {
    -- inside a thread, can spawn more threads
    spawn _ = logToFile("subtask started");
    let subResult int;
    spawn subResult = computeSubtask();
    join subResult;
    return subResult;
}

let result int;
spawn result = processData();
join result;
```

### Spawn and the Visual Graph

```
[@[export] const main (args [_]string)]
    │
    [spawn | _ | backgroundTask]────┐  ← Dotted line: no join
    │                               │
    [let result int]                │
    │                               │
    [spawn | result | heavyWork]──┐ │  ← Solid line: will be joined
    │                             │ │
    ...continue execution         │ │
    │                             │ │
    [join | result] ──────────────┘ │  ← Merge point
    │                               │
    [end] ──────────────────────────┘  ← Unjoined threads terminate
```

### Performance Guidelines

| Use Case                | Syntax                  | Count     | Reasoning                |
| ----------------------- | ----------------------- | --------- | ------------------------ |
| Fire and forget logging | `spawn _ = log()`       | Dozens    | No need to wait          |
| Background cleanup      | `spawn _ = gc()`        | Few       | Runs independently       |
| CPU-bound computation   | `spawn result = work()` | CPU cores | Need result, use threads |
| Many independent tasks  | `spawn _ = task()`      | Dozens    | Threads have overhead    |

### Complete Example

```lucid
import std.io as io
import std.http as http

-- Parallel processing with results
const processImages (images [*]Image) -> [*]ProcessedImage = {
    let results [*]ProcessedImage = [];

    -- Spawn a thread for each image
    for _, img Image in images {
        let processed ProcessedImage;
        spawn processed = imageCompiler(img);
        arr:append<ProcessedImage>(results)(processed);
    }

    -- Wait for all images to be processed
    let output [*]ProcessedImage = [];
    for _, processed ProcessedImage in results {
        join processed;
        arr:append<ProcessedImage>(output)(processed);
    }

    return output;
}

-- Mixed: fire-and-forget + joinable
@[export] const main () -> int = {
    -- Fire and forget: analytics and logging
    spawn _ = sendAnalytics("app_started");
    spawn _ = logToFile("main started");

    -- Fire and join: parallel computations
    let userData string;
    let productData string;

    spawn userData = fetchUserData();
    spawn productData = fetchProductData();

    -- Do some work while fetches run
    let config Config = loadConfig();

    -- Wait for both fetches
    join userData, productData;

    -- Process results
    const user User = parseUser(userData);
    const products [*]Product = parseProducts(productData);

    io:printl("User: " + user.name);
    io:printl("Products loaded: " + stringFromInt(len(products)));

    -- Fire and forget: final cleanup
    spawn _ = logToFile("main completed");

    return 0;
}
```

### Spawn vs Async — Quick Reference

| Aspect          | `spawn`                  | `async`                     |
| --------------- | ------------------------ | --------------------------- |
| Model           | Parallelism (OS threads) | Concurrency (event loop)    |
| Scheduling      | Preemptive               | Cooperative                 |
| Overhead        | ~1MB stack               | ~few KB                     |
| Capacity        | CPU cores (dozens)       | 10,000+                     |
| Best for        | CPU work, blocking I/O   | I/O, network, file I/O      |
| Join with       | `join`                   | `await`                     |
| Fire and forget | `spawn _ =`              | N/A (must await or warning) |
| Data sharing    | Needs locks              | Safe at yield points        |