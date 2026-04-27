# Luc — Grammar Reference

> **Scope of this file:** Formal grammar rules for the Luc parser.
> Code examples are in `LUC_EXAMPLES.md`. Project identity is in `LUC_PROJECT_OVERVIEW.md`.

## Notation

```
rule        := ...          -- definition
A B                         -- sequence
A | B                       -- alternative
( A )                       -- grouping
[ A ]                       -- optional (0 or 1)
{ A }                       -- zero or more
A+                          -- one or more
'token'                     -- literal terminal
```

## Separators

Commas `,` and semicolons `;` are **optional** throughout — Luc uses newlines and
block structure to delimit constructs (Go / scripting style). The parser must
accept them but never require them.

## Top-Level Structure

```
program         := package_decl { top_level_decl }

package_decl    := 'package' IDENTIFIER

top_level_decl  := { attribute } actual_decl
                   -- Zero or more '@' attributes may precede any declaration.
                   -- Attributes are consumed and attached to the declaration that follows.

actual_decl     := module_decl     -- optional re-export manifest (one per package)
                 | use_decl
                 | enum_decl        -- enum keyword:   named constant set, integer-backed
                 | struct_decl      -- struct keyword: data definition
                 | trait_decl       -- trait keyword:  method contract / constraint
                 | type_decl        -- type keyword:   alias (primitive, named, union, array, func)
                 | impl_decl        -- top-level only: same file and scope as struct
                 | from_decl        -- top-level only: type conversion entry point for a named struct
                 | var_decl
                 | func_decl        -- shorthand: top-level let/const with func type
```

### Entry Point (main)

The language uses a standard `main` function as the execution entry point. The parsing and AST do not treat `main` differently than other functions, but the semantic pass expects it to follow a specific signature. A typical systems-level `main` looks like this:

```luc
export const main () int { ... }
-- or with command-line arguments:
export const main (args []string) int { ... }
```

### Compilation Mode Directives (`@aot` / `@jit`)

The `@aot` and `@jit` attributes on the `main` function tell the compiler which
execution model to use. They are mutually exclusive — using both on the same
declaration is a semantic error.

```
@aot   -- ahead-of-time compilation: produces a native binary at build time
@jit   -- just-in-time compilation:  compiles and executes at runtime via LLVM JIT
```

**Rules:**

- Only valid on the `main` entry point — using `@aot` or `@jit` on any other
  declaration is a semantic error.
- Mutually exclusive — `@aot` and `@jit` cannot both appear on the same declaration.
- When neither is present, the compiler uses its default mode (determined by
  build configuration).

```luc
-- AOT: compile to a native binary
@aot
export const main () int = {
    return 0
}

-- JIT: compile and run via LLVM JIT
@jit
export const main () int = {
    return 0
}
```

## Module System

Every `.luc` file is a module. The module's identity is its file path relative
to the package root — no declaration is needed inside the file. Modules are
flat; nesting is not supported.

### Visibility — three tiers

| Keyword | Scope | Access |
|---|---|---|
| (none) | **File** | Only visible within the same `.luc` file. |
| `pub` | **Package** | Visible to all files sharing the same `package` IDENTIFIER. |
| `export` | **World** | Visible to external consumers of the package. |

`pub` on a top-level declaration (`struct`, `trait`, `type`, `let`, `const`, `enum`, `from`) makes it
accessible to other files in the same package. Without `pub`, a declaration is
private to its file.

`export` makes a top-level declaration accessible to external consumers importing this package.

`pub impl` vs `export impl` vs `impl` controls method accessibility: `export impl` methods are callable externally; `pub impl` methods are callable by anyone holding the value within the package; bare `impl` methods are callable only within the file.

`pub from` vs `export from` vs `from` controls converter accessibility: `export from` converters are callable externally via `TypeName(expr)`; `pub from` converters are callable within the package; bare `from` converters are callable only within the file.

### API Manifests (Re-Exports)

To satisfy the desire for a "single point of truth" or a "curated API" without forcing a specific file naming convention, you can use `export` to modify `use` statements:

```luc
-- math/api.luc (or any other file in the math package)
package math

export use math.vec2         -- Re-export all pub items from vec2
export use math.matrix.Mat2  -- Granular re-export of a single type
```

### Rules

- One `package` declaration per file, first non-comment line
- `pub` and `export` are **Top-Level Modifiers** only. They are **ILLEGAL** inside blocks, function bodies, or local assignments.
- If a package has *zero* `export` modifiers or re-exports, it is a **Private Package** (nothing is implicitly exported).
- Circular imports are a semantic error
- `as IDENTIFIER` gives a local alias to the imported path

### Example

```luc
-- math/vec2.luc
package math

pub struct Vec2 { x float  y float }   -- shared within math package

pub impl Vec2 {                        -- methods callable within package
    length () float = { ... }
}

export impl Vec2 {                     -- methods callable externally
    normalize () Vec2 = { ... }
}

impl Vec2 {                            -- private helpers, file-only
    validate () bool = { ... }
}

const PI float = 3.14159                 -- file-private, not pub
```

```luc
-- consumer
use math             -- gets only what the package exports via `export`
use math.vec2        -- can also import a specific file directly
use math as m        -- local alias: m.Vec2
```

## Types

### Type Grammar

```
type            := base_type [ generic_args ] [ nullable_suffix ]
                 | union_type
                 | ref_type
                 | ptr_type
                 | array_type
                 | func_type

base_type       := primitive_type
                 | IDENTIFIER              -- named / user-defined type

primitive_type  := 'bool'
                 | 'byte'  | 'short'  | 'int'  | 'long'
                 | 'ubyte' | 'ushort' | 'uint' | 'ulong'
                 | 'int8'  | 'int16'  | 'int32'  | 'int64'
                 | 'uint8' | 'uint16' | 'uint32' | 'uint64'
                 | 'float' | 'double' | 'decimal'
                 | 'string' | 'char'
                 | 'any'

-- Nullable suffix
nullable_suffix := '?'

-- Union type  (int | string)
union_type      := type '|' type { '|' type }

-- Reference   (&T)
ref_type        := '&' type

-- Raw pointer (*T) — only valid on declarations carrying @extern or via intrinsics.
-- See "The Sealed Conduit Model" below for rules.
ptr_type        := '*' type

-- Array types — three distinct kinds:
--
--   [N]T    fixed array:   size is a compile-time constant, stack/inline allocated
--   []T     slice:         a view into an array, no ownership, not growable
--   [*]T    dynamic array: heap-owned, growable, length and capacity tracked
--
--   multidimensional: [][*]float  |  [4][4]float  |  [*][*]int
--
array_type      := '[' INT_LITERAL ']' type      -- fixed:   [100]int
                 | '[' ']' type                  -- slice:   []int
                 | '[' '*' ']' type              -- dynamic: [*]int
                 | array_type array_type          -- nested:  [][*]float

-- Generics    (Buffer<T>, Map<K, V>)
generic_args    := '<' type { [','] type } '>'
```

### Nullable Rules

```
-- Standalone value: ? attaches directly to the type
let x int? = nil

-- Function return: ? outside parens = nullable return value
let f (num int) int?            -- returns int? (nullable int)

-- Nullable function: ? wraps the entire function type — note outer parens
let f ((num int) int)?          -- the function itself is nullable
```

## Value and Reference Semantics

Every type in Luc is either **owned** or **borrowed**. This distinction is made
explicit through syntax — bare `T` always means owned, `&T` always means
borrowed. The compiler enforces ownership rules at the semantic pass without
requiring annotations beyond what is already written in the type.

### Owned types — copied on assignment

An owned type is fully contained in the variable that holds it. Assigning to
another variable or passing to a function produces a fully independent copy.
Neither side can observe mutations made through the other.

| Type | Syntax | Storage | On assignment |
|---|---|---|---|
| Primitives | `bool` `int` `float` `char` … | stack / inline | full copy |
| Enum | `Direction.North` | integer (`byte` / `short`) | full copy |
| Fixed array | `[N]T` | stack / inline | full element copy |
| Slice | `[]T` | fat pointer (`ptr + len + cap`) | copies the view header — both slices share the underlying buffer |
| Dynamic array | `[*]T` | heap-owned buffer | full deep copy of the buffer |
| String | `string` | heap-owned sequence | full deep copy |
| Struct | `Vec2` `Player` … | inline / stack | full deep copy — see below |
| Named function | `add` `Vec2:normalize` | function pointer | pointer copy (no captured state) |
| Closure | `add(10)` `(x int) int { … }` | heap-allocated environment | copies the reference to the environment |

### Struct deep copy

Struct assignment always produces a **fully independent value**. Every field
is recursively copied according to its own type semantics — owned fields are
cloned, borrowed fields copy their reference only.

The rule is derived from ownership, not from a special struct rule:

- **Owned fields** (`int`, `[*]T`, `string`, nested `struct`) — their data is
  cloned. The new struct owns its own independent copy of every owned field.
- **Borrowed fields** (`&T`) — the reference is copied. Both the original and
  the copy hold a reference to the same external value. The external value is
  not owned by either struct and is not cloned.

```luc
struct Player {
    score  int          -- owned: cloned
    items  [*]string    -- owned: buffer is deep-copied
    world  &World       -- borrowed: reference is copied, World is not cloned
}

let a Player = Player { score = 10  items = ["sword"]  world = &w }
let b Player = a

-- b.score  is 10 — independent int
-- b.items  is a new buffer ["sword"] — mutating b.items does not affect a.items
-- b.world  points to the same World as a.world — it is borrowed, not owned
```

This means a developer never has to inspect a struct's fields to know whether
assignment is safe — owned data is always cloned, borrowed data is always
explicit from the `&` in the field's type.

### Borrowed types — share without copying

A borrowed reference `&T` gives access to a value owned elsewhere without
copying it. The borrow does not affect the owner's lifetime. Using `const` on
a borrowed reference produces a read-only view.

```luc
let a Player = Player { … }

-- mutable shared reference — mutations through ref are visible via a
let ref &Player = a

-- read-only shared reference — semantic pass rejects any field mutation through ref
const ref &Player = a
```

| Declaration | Copies? | Field mutation allowed? | Who owns the data? |
|---|---|---|---|
| `let b Player = a` | ✅ deep copy | ✅ on `b`'s own fields | `b` owns its copy |
| `let ref &Player = a` | ❌ shared | ✅ mutations visible through `a` | `a` owns it |
| `const ref &Player = a` | ❌ shared | ❌ read-only view | `a` owns it |

### Circular references

When a struct field is of type `&T`, the struct borrows the referenced value
— it does not own it. This makes linked and tree structures straightforward:

```luc
struct Node {
    value int
    next  &Node?    -- borrowed reference to the next node, nullable
}

let a Node = Node { value = 1  next = nil }
let b Node = a     -- deep copy: b.value = 1 (cloned), b.next = nil (reference copied)
```

Copying a `Node` copies its `value` and copies the `next` reference — it does
not recursively clone the entire linked list. This is correct because `next`
is borrowed (`&Node?`), not owned.

### Function values and closures

Named functions and impl methods carry no captured state — they are plain
function pointers and behave as value types.

Closures (partial applications and anonymous functions that capture variables)
hold a heap-allocated environment. Assigning a closure copies the reference to
that environment — both variables then share the same captured state.

```luc
let addTen = add(10)    -- closure: captures a = 10 in a heap environment
let alias  = addTen     -- alias holds the same environment as addTen
                        -- both call the same captured a = 10
```

Closure capture follows ownership:
- **Value-type variables** are copied into the environment at the point of
  capture — later mutations to the original do not affect the closure.
- **Reference-type variables** (`&T`, `[]T`, `[*]T`) — the reference is
  captured. Mutations visible through the reference are visible inside the
  closure and vice versa.

### `any` and boxing

A value stored in `any` is boxed — wrapped in a runtime container that carries
its type tag alongside the value. Ownership is preserved through the box:

- Owned types placed into `any` are deep-copied into the box. The box owns
  the copy.
- Borrowed references (`&T`) placed into `any` store the reference. The box
  does not own the referenced value.

Boxing behaviour (stack-allocated small box vs heap pointer) is a compiler
implementation detail — the programmer observes only the ownership contract
above.

## The Sealed Conduit Model (Raw Pointers)

Raw pointers (`*T`) in Luc are treated as **sealed conduits**. They provide zero unsafe surface by default. You can carry them, pass them to external functions, and check them for `nil`, but you cannot work with the data they point to without an explicit crossing operation.

### Allowed Operations (Safe)

1. **Storage**: Store a pointer value in a variable or struct field.
2. **Passing**: Pass a pointer to an `@extern` function.
3. **Nil Check**: Compare a pointer to `nil` (`== nil`, `!= nil`).
4. **Intrinsics**: Pass to pointer management intrinsics (see below).
5. **Printing**: Print the pointer value (memory address) for debugging.

### Forbidden Operations (Compiler Error)

- **Dereferencing**: `*ptr` is not supported for raw pointers.
- **Field Access**: `ptr.field` must cross to a safe reference first.
- **Indexing**: `ptr[i]` must cross to a slice or reference first.
- **Arithmetic Operators**: `ptr + 4` or `ptr - 4` are prohibited.
- **Assignment**: `*ptr = value` is prohibited.

### Boundary Crossing (Intrinsics)

To work with memory, you must use explicit intrinsics that mark the "unsafe" crossing point in your code:

- `@ptrToRef(T, ptr)`: Asserts that `ptr` is valid and returns a Luc reference (`&T`) to the memory. This is the primary way to "unseal" a pointer.
- `@refToPtr(ref)`: Converts a safe Luc reference (`&T`) back into a raw pointer (`*T`).
- `@ptrOffset(ptr, n)`: Perfroms pointer arithmetic; returns a new raw pointer offset by `n` elements.
- `@ptrDiff(p1, p2)`: Returns the signed distance (in elements) between two pointers as an `int64`.

```luc
-- Example: Working with a C-allocated buffer
const buf *uint8? = malloc(1024)
if buf == nil { panic("allocation failed") }

-- Explicitly cross the boundary to work with it as a reference
const ref &uint8 = @ptrToRef(&uint8, buf)

-- Now use normal Luc semantics
ref = 0xFF

-- Pointer arithmetic for the next element
const next *uint8? = @ptrOffset(buf, 1)
```

## Variable Declaration

```
var_decl        := decl_keyword IDENTIFIER type_ann [ '=' expr ]

decl_keyword    := 'let' | 'const'

type_ann        := type                    -- bare type, no colon needed, always required
                   -- e.g.  let x int = 5
                   -- e.g.  let name string? = nil
```

### Declaration Semantics

| Keyword | Reassignable | Mutable in place | Value known at | Nil allowed |
|---|---|---|---|---|
| `let` | ✅ | ✅ | runtime | ✅ |
| `const` | ❌ | ❌ | **compile time** | ❌ |

`let` is the general-purpose variable — fully flexible, reassignable and mutable in place.
`const` declares a compile-time constant. The initialiser must be a constant expression —
a literal, another `const` name, an enum variant, or arithmetic over those. Function calls,
struct literals, closures, and any runtime-computed value are rejected by the semantic pass.

```luc
-- valid const initialisers
const MAX_VERTICES int   = 65536
const PI           float = 3.14159
const HALF_PI      float = PI / 2.0          -- arithmetic over consts: OK
const NEG_PI       float = -PI                -- unary negation of a const: OK
const STAGE        int   = ShaderStage.Vertex -- enum variant: OK
const AS_FLOAT     float = float(MAX_VERTICES) -- safe type conversion: OK

-- invalid const initialisers — semantic error
const x int    = readInput()       -- ERROR: function call is not a compile-time constant
const v Vec2   = Vec2 { x = 0.0 } -- ERROR: struct literal is not a compile-time constant
const n int    = someLetVar        -- ERROR: let variable is not a compile-time constant
```

## Functions

Functions are **first-class values**. A function declaration is syntactic sugar
for assigning a function body (or anonymous function) to a variable.

**No overloading** — a function name within a scope always maps to exactly one
signature. Declaring two functions with the same name but different parameters
is a semantic error. Use different names, currying, union parameters, or
nullable parameters to express the same intent.

### Function Type

```
func_type       := '(' [ param_list ] ')' [ return_type ]
                 | '(' '(' [ param_list ] ')' [ return_type ] ')' '?'
                   -- second form: entire function type is nullable
                   -- e.g.  ((num int) int)?

return_type     := type
                 | '(' type ')' '?'        -- nullable return grouped explicitly
```

### Function Declaration (shorthand — preferred)

```
func_decl       := decl_keyword IDENTIFIER [ generic_params ]
                   param_group { param_group } [ return_type ] [ '?' ]
                   '=' func_body

param_group     := '(' [ param_list ] ')'
                   -- single group = normal function
                   -- multiple groups = curried function (see Currying section)

func_body       := block                   -- body block: = { stmts }
                 | anon_func               -- explicit anonymous function: = (params) ret { stmts }
```

### Anonymous Function

```
anon_func       := [ async_mod ] '(' [ param_list ] ')' [ return_type ] block

async_mod       := 'async'
```

### Parameter List

```
param_list      := param { [','] param } [ [','] variadic_param ]

param           := IDENTIFIER type         -- positional: name then type (Go style)

variadic_param  := IDENTIFIER '...' type   -- e.g.  args ...int
```

### Examples in Grammar Terms

```
-- Shorthand (body block — preferred), '=' assigns the block as the function body
-- let: reassignable and mutable — a function variable whose body can be replaced
let f (num int) int = { return num + 1 }

-- Reassignment — assign a new body to an existing let function variable
f = { return 42 }

-- const: body is permanently bound, cannot be reassigned
const f (num int) int = { return num + 1 }

-- Explicit anonymous function (identical semantics, more verbose)
let f (num int) int = (num int) int { return num + 1 }

-- Nullable return — ? on the return type
let parse (s string) int? = { ... }

-- Nullable function itself — outer parens wrap the whole signature, then ?
let handler ((req Request) Response)? = nil

-- Async function
let fetch (url string) string = async (url string) string { ... }

let fetch (url string) string = async { ... }
```

## Struct Declaration

```
struct_decl     := [ 'pub' ] 'struct' IDENTIFIER [ generic_params ] '{' { field_decl } '}'

field_decl      := IDENTIFIER type [ '=' expr ]    -- name then type
                                                   -- optional default value

generic_params  := '<' generic_param { [','] generic_param } '>'

generic_param   := IDENTIFIER                              -- unconstrained:          T
                 | IDENTIFIER ':' IDENTIFIER               -- single constraint:      T : Drawable
                 | IDENTIFIER ':' constraint_list          -- multiple constraints:   T : Drawable + Serializable

constraint_list := IDENTIFIER { '+' IDENTIFIER }
```

### Struct Literals

A struct literal constructs a value of the struct type. Fields are assigned
using `=`. Fields with default values may be omitted — they take the declared
default. Fields without defaults must be provided.

```
struct_literal  := IDENTIFIER '{' { field_init } '}'
                 | IDENTIFIER generic_args '{' { field_init } '}'

field_init      := IDENTIFIER '=' expr
                   -- name = value  (always = never :)
```

### Field Assignment

Fields of a struct held by a `let` variable can be assigned directly:

```
field_assign    := expr '.' IDENTIFIER '=' expr
                   -- only valid if the containing variable is 'let'
                   -- const fields are not reassignable
```

Examples:
```luc
-- Basic struct declaration
struct Vec2 {
    x float
    y float
}

-- Struct with default values
struct Color {
    r float = 1.0
    g float = 1.0
    b float = 1.0
    a float = 1.0
}

pub struct Vertex {
    pos   Vec3
    color Vec4
    uv    Vec2
}

-- Generic unconstrained
struct Vec2<T> {
    x T
    y T
}

-- Single constraint
struct Scene<T : Drawable> {
    objects []T
}

-- Multiple constraints
struct Cache<K : Hashable + Comparable, V> {
    keys   []K
    values []V
}

-- Struct literal: field = value  (always '=', never ':')
let origin Vec2  = Vec2  { x = 0.0  y = 0.0 }
let white  Color = Color {}                      -- all fields take their defaults
let v Vertex     = Vertex { pos = Vec3 { x = 0.0  y = 0.0  z = 0.0 }
                            color = Vec4 { r = 1.0  g = 1.0  b = 1.0  a = 1.0 }
                            uv = Vec2 { x = 0.0  y = 0.0 } }

-- Generic struct literal
let p Vec2<int> = Vec2<int> { x = 1  y = 2 }

-- Field access and assignment
let pos float = origin.x          -- read field: plain '.'
origin.x = 5.0                    -- write field: only valid if origin is 'let'

-- Note struct patterns in 'match' use ':' — that is pattern syntax, not assignment
-- match point { Vec2 { x: 0.0, y: 0.0 } -> ... }  ← ':' here is a pattern separator
-- struct literals use '=' everywhere else
```

## Member Access

Luc uses two distinct access operators that reflect where a member was defined.
The compiler knows from the AST whether a name came from a `struct` field or an
`impl` method and enforces the rules accordingly.

```
member_access   := expr '.' IDENTIFIER     -- data member: field from struct body
                 | IDENTIFIER ':' IDENTIFIER  -- behavior member: method from impl block
```

### `.` — Data member access

Accesses a field declared in a `struct {}` body. Data members are reassignable
if the containing variable is `let`. A data member can hold any type, including
a function type.

```luc
v.x              -- read data field
v.x = 5.0        -- write data field (only valid if v is let)
player.health    -- read numeric field
player.onClick   -- read a field that holds a function value
player.onClick = () { ... }   -- replace a function-typed field (valid, it's data)
```

### `:` — Behavior member (impl namespace access)

`TypeName:methodName` accesses a method from a type's `impl` block. The type
name before `:` is the **namespace** — it identifies which impl the method
comes from. The result is a plain function reference. Behavior members are
**never reassignable**.

```luc
Vec2:normalize     -- function reference to normalize from Vec2's impl
Vec2:length        -- function reference to length from Vec2's impl

-- explicit typed declaration required (luc is explicitly typed)
let fn () float = Vec2:length      -- fn is a () float function reference
let fn (v Vec2) Vec2 = Vec2:normalize  -- fn takes a Vec2, returns Vec2

-- behavior is not reassignable
Vec2:length = ...   -- ERROR: impl methods cannot be reassigned
```

### Methods are just functions

A method from `impl` is a regular function — it can be passed as an argument,
stored in a typed variable, or used as a pipeline step. The difference from
data members is purely about where it was declared and whether it can be replaced.

```luc
-- use an impl method directly in a pipeline
-- -> passes upstream v as the first argument to Vec2:normalize
let v Vector = Vector { x = 1.0  y = 1.0 }
v -> Vec2:normalize

-- a data member that holds a function behaves identically in a pipeline
let transform (v Vec2) Vec2 = player.onTransform   -- read the data field
v -> transform                                      -- call it via pipeline
```

An enum declares a named set of constants under a shared type. Each variant
is a named member with no data payload. Enums are stored as integers internally
— the compiler picks the smallest integer type that fits all variants (`byte`
for ≤ 255 variants, `short` for more). The programmer never sees the integer
representation.

Enums are distinct from `const` declarations — see the note below.

```
enum_decl       := [ 'pub' ] 'enum' IDENTIFIER '{' enum_variant { [','] enum_variant } '}'

enum_variant    := IDENTIFIER                      -- auto-assigned integer value
                 | IDENTIFIER '=' INT_LITERAL      -- explicit integer value
```

### Rules

- Enum variants are accessed via `EnumName.VariantName` — dot syntax, never bare
- Without explicit values, variants are assigned integers starting from `0`
- Explicit values may be assigned to any variant; unassigned variants after an
  explicit value continue incrementing from that value
- Enum types are compatible with `match` and the `is` operator
- `pub` makes the enum and all its variants visible to other files in the package

### `const` vs `enum` — key difference

| | `const` | `enum` |
|---|---|---|
| Groups related values | ❌ individual declarations | ✅ all variants under one type |
| Type safety | weak — `type ID = int` is just an alias | strong — `Direction` is its own type |
| Exhaustiveness in `match` | ❌ compiler cannot check | ✅ semantic pass can verify |
| Memory | size of the value type | integer, compiler-chosen size |
| Use case | single named constant | closed set of named states or options |

```luc
-- const — single named value, no grouping, no type enforcement
const MAX_VERTICES int = 65536
const PI           float = 3.14159

-- enum — closed set of named variants under a single type
-- the compiler knows the complete set; match can be checked for exhaustiveness
pub enum Direction {
    North
    South
    East
    West
}

pub enum ShaderStage {
    Vertex   = 0x01    -- explicit values, useful for Vulkan flag bits
    Fragment = 0x02
    Compute  = 0x04
    Geometry = 0x08
}

pub enum Status {
    Ok
    Warn
    Error
}
```

### Usage

```luc
-- access via dot syntax
let dir Direction = Direction.North
let stage ShaderStage = ShaderStage.Fragment

-- match on enum — semantic pass can verify exhaustiveness
let describe (dir Direction) string = match dir {
    Direction.North -> "north"
    Direction.South -> "south"
    Direction.East  -> "east"
    Direction.West  -> "west"
    default         -> "unknown"
}

-- is operator with enum
if stage is ShaderStage.Fragment {
    bindFragmentShader()
}
```

## `type` keyword

`type` declares a named alias. `=` is always required as the separator between
the name and the right-hand side. The right-hand side may be a primitive, a
named type, a union, an array, a ref, a raw pointer, or a function type.
Inline struct bodies are **not allowed** — use a named `struct` declaration
instead, which also enables `impl` and trait conformance.

```
type_decl       := [ 'pub' ] 'type' IDENTIFIER [ generic_params ] '=' type_alias_rhs

type_alias_rhs  := primitive_type                  -- type ID          = int
                 | IDENTIFIER                      -- type UserID      = ID
                 | union_type                      -- type Number      = int | float
                 | array_type                      -- type Matrix   = [][*]float
                                                   -- type ByteBuf  = []byte
                                                   -- type VertBuf  = [*]Vertex
                 | ref_type                        -- type IntRef      = &int
                 | ptr_type                        -- type RawBuf      = *uint8   (@extern only)
                 | func_type                       -- type Callback    = (event Event) bool
                                                   -- type Handler     = (req Request) Response?
                                                   -- type Transform<T> = (value T) T

-- NOT allowed:  type Vertex = struct { ... }  ->  use  struct Vertex { ... }
```

### Examples

```luc
-- Primitive alias
type ID     = int
type Score  = float

-- Named alias
type UserID = ID
type Mesh   = Vertex

-- Union alias
type Number  = int | float
type Result  = string | int | nil

-- Array alias
type Matrix  = [][*]float   -- slice of dynamic float arrays
type ByteBuf = []byte       -- slice of bytes
type VertBuf = [*]Vertex    -- dynamic array of vertices

-- Function type alias
type Callback     = (event Event) bool
type Handler      = (req Request) Response?
type Transform<T> = (value T) T

-- Nullable function alias
type MaybeHandler = ((req Request) Response)?
```

### Alias semantics

A `type` alias introduces a new name for an existing shape — it does not create
a new distinct type. At the semantic level `ID` and `int` are interchangeable.
For a distinct nominal type that supports `impl` and trait conformance, declare
a `struct` instead.

## Trait

A trait declares a named method contract — a set of method signatures a type
must provide to satisfy the constraint. Traits contain **no field declarations**
and **no method bodies** — they are pure shape definitions.

```
trait_decl      := [ 'pub' ] 'trait' IDENTIFIER [ generic_params ] '{' { trait_method } '}'

trait_method    := IDENTIFIER '(' [ param_list ] ')' [ return_type ]
                   -- signature only — no '=' and no body
```

### Rules

- Traits are top-level only — never nested inside functions or blocks
- No field declarations inside a trait — method signatures only
- No default implementations — every method must be provided by the impl
- A struct satisfies a trait by declaring `impl StructName : TraitName { ... }`
- The impl block must provide **every** method in the trait — missing methods are a semantic error reported at the impl site
- A struct may satisfy multiple traits across separate impl blocks
- Trait conformance is **explicit** — the `: TraitName` annotation is required

### Method name collision across traits

When a struct implements two traits that share a method name, two cases apply:

**Same name, same signature — one implementation satisfies both:**

If two traits declare the same method name with identical signatures, a single
implementation in one `impl` block satisfies both traits simultaneously. This
is not an error — both traits agree on the contract and one body fulfils it.

```luc
pub trait Drawable {
    draw ()
}

pub trait Renderable {
    draw ()    -- same name, same signature as Drawable.draw
}

-- one impl block satisfies both traits
pub impl Circle : Drawable {
    draw () = { io.printl("drawing circle") }
}

pub impl Circle : Renderable {
    -- draw () is already provided above — both traits are satisfied
    -- no additional implementation needed
}
```

**Same name, different signature — semantic error:**

If two traits declare the same method name with different signatures, implementing
both on the same struct is a conflict. The semantic pass reports an error at the
impl site. The resolution is to use different method names in the trait declarations.

```luc
pub trait Drawable {
    draw ()
}

pub trait DrawableAt {
    draw (x float, y float)   -- same name, different signature
}

-- ERROR: Circle cannot implement both — draw() conflicts with draw(float, float)
pub impl Circle : Drawable  { draw ()                  = { ... } }
pub impl Circle : DrawableAt { draw (x float, y float) = { ... } }
-- semantic error: duplicate method name 'draw' with conflicting signatures
```

### No function overloading

Luc does not support function overloading — a function name within a scope
always maps to exactly one signature. This is intentional:

- **First-class functions** — overloaded names cannot be passed as values
  (`let fn = Circle:draw` would be ambiguous if `draw` had multiple signatures)
- **Pipeline operator** — step type resolution depends on a single unambiguous
  function type per name
- **Functional clarity** — one name, one type, one behaviour

Use different names, currying, union type parameters, or nullable parameters
instead of overloading:

```luc
-- instead of overloading draw():
let draw        ()                 = { ... }   -- default position
let drawAt      (x float, y float) = { ... }   -- explicit position
let drawAtPoint (pos Vec2)         = { ... }   -- Vec2 position

-- or use a union/nullable parameter for optional args
let draw (pos Vec2?) = {
    if pos != nil { ... } else { ... }
}
```

```luc
pub trait Drawable {
    draw   ()
    bounds () Rect
}

pub trait Serializable {
    serialize   () string
    deserialize (s string) any
}

-- Generic trait
pub trait Comparable<T> {
    compareTo (other T) int    -- negative / zero / positive
}

pub trait Hashable {
    hash   () uint64
    equals (other any) bool
}
```

## Impl Block

Binds named behaviors to a struct type. Methods inside `impl` are **not variable
declarations** — no `let`/`const`. Visibility is controlled at the block
level — `pub impl` for public methods, bare `impl` for private. An impl block
may optionally declare trait conformance with `: TraitName`.

Multiple blocks of each kind are allowed and merge at semantic time, subject to
these rules:

- `impl` must be in the **same file** as the struct declaration
- `impl` must be at **top-level scope** — never nested inside functions or blocks
- `impl` must appear **after** the struct declaration in the file
- Duplicate method names across merged blocks are a **semantic error** — each
  method name must be unique within the struct's complete impl surface
- If `: TraitName` is present, every method in that trait must be implemented
  in the same block — a partial implementation is a semantic error
- If two `: TraitName` blocks share a method name with the same signature, the
  single existing implementation satisfies both — no duplication required
- If two `: TraitName` blocks share a method name with different signatures,
  this is a semantic error — see Trait section for resolution
- For generic structs, the `impl` block must use the **exact same generic signature** as the struct declaration (e.g., `impl Scene<T : Drawable>`). Concrete types are not allowed in the `impl` generic parameters.

```
impl_decl       := [ visibility_mod ] 'impl' IDENTIFIER [ generic_params ] [ ':' trait_ref ]
                   '{' { method_decl } '}'

visibility_mod  := 'pub' | 'export'

trait_ref       := IDENTIFIER [ generic_args ]     -- e.g.  : Drawable
                                                   --        : Comparable<int>

method_decl     := IDENTIFIER '(' [ param_list ] ')' [ return_type ] '=' func_body
                   -- no per-method visibility prefix — visibility is set at the impl block level
                   -- export impl -> methods callable externally
                   -- pub impl    -> methods callable by anyone holding the value within the package
                   -- impl        -> methods callable only within the file
                   -- multiple blocks of each kind are allowed and merge at semantic time
```

### Examples

```luc
pub impl Vec2 {
    length () float = { return (x*x + y*y) -> sqrt }
    dot (other Vec2) float = { return x*other.x + y*other.y }
}

pub impl Vec2 {
    -- additional public methods — valid, merges with above
    scale (factor float) Vec2 = { return Vec2 { x = x * factor  y = y * factor } }
}

impl Vec2 {
    -- private helpers, callable only within the package
    isZero () bool = { return x == 0.0 and y == 0.0 }
}

-- Trait conformance
pub impl Circle : Drawable {
    draw   ()        = { ... }
    bounds () Rect   = { ... }
}

-- Multiple traits across separate blocks
pub impl Point : Drawable {
    draw   ()        = { ... }
    bounds () Rect   = { ... }
}

pub impl Point : Hashable {
    hash   () uint64       = { ... }
    equals (other any) bool = { ... }
}

-- Generic impl with constraint
pub impl Scene<T : Drawable> {
    drawAll () = {
        for obj in objects { obj.draw() }
    }
}
```

## From Declaration

`from` declares a type conversion entry point for a named struct. It is a
**top-level declaration** — the same level as `impl`, `struct`, and `trait`.
Visibility follows the same three-tier model as every other top-level declaration.

```
from_block      := [ visibility_mod ] 'from' IDENTIFIER '{' from_entry* '}'

from_entry      := param_group { param_group } IDENTIFIER '=' func_body
                   -- ^^^^^^^^^^^^^^^^^^^^^^^^^^^ ^^^^^^^^^^
                   -- source param(s)              return type (must match target)

visibility_mod  := 'pub' | 'export'
```

### Rules

- `from` must be at **top-level scope** — never inside blocks, functions, or impl bodies
- `from` must appear in the **same file** as the target struct declaration
- The return type name must match the target struct name — enforced by the semantic pass
- Multiple `from` declarations for the same target type are valid as long as each
  accepts a **different source parameter type** — duplicate source types are a semantic error
- Visibility is independent of the target struct's visibility — a `pub struct` may have
  file-private converters, and a private struct may have `pub from` if needed within the package
- `TypeName(expr)` call syntax at use sites dispatches to the matching `from` declaration
  based on the type of `expr` — this is not a constructor, it is a named conversion

### Examples

```luc
-- temperature.luc
package math

pub struct Celsius    { value float }
pub struct Fahrenheit { value float }
pub struct Kelvin     { value float }

-- Grouped conversions for Fahrenheit
export from Fahrenheit {
    -- Conversion from Celsius
    (c Celsius) Fahrenheit = {
        return Fahrenheit { value = c.value * 9.0 / 5.0 + 32.0 }
    }

    -- Conversion from Kelvin
    (k Kelvin) Fahrenheit = {
        return Fahrenheit { value = (k.value - 273.15) * 9.0 / 5.0 + 32.0 }
    }
}

-- Package-private conversions for Kelvin
pub from Kelvin {
    (c Celsius) Kelvin = {
        return Kelvin { value = c.value + 273.15 }
    }
}
```

### Desugaring and Implicit Casting

When a `from` declaration exists for a target type `T` accepting a source type `S`, the compiler provides **automatic implicit casting** (desugaring) in several contexts.

#### 1. Variable Declarations
A `let` or `const` declaration with an explicit type `T` can be initialized with an expression of type `S`.

```luc
let rawSecs Seconds = Seconds { val = 3600 }
let m Minutes = rawSecs  -- Desugars to: let m Minutes = Minutes(rawSecs)
```

#### 2. Function Arguments
When a function parameter is declared as type `T`, and a value of type `S` is passed, the compiler desugars the argument at the call site.

```luc
const doubleKm (km Kilometers) float = { ... }
let d Meters = Meters { val = 5000.0 }

doubleKm(d)  -- Desugars to: doubleKm(Kilometers(d))
```

#### 3. Function Return Values
A function returning type `S` can be used to initialize or assign to a variable of type `T`.

```luc
const freezingPoint () Celsius = { ... }
let f Fahrenheit = freezingPoint()  -- Desugars to: let f Fahrenheit = Fahrenheit(freezingPoint())
```

#### 4. Assignment Statements
Reassignment of an existing variable of type `T` with a value of type `S` triggers the same desugaring.

```luc
let currentTemp Fahrenheit = Fahrenheit { val = 70.0 }
let newReading Celsius = Celsius { val = 37.0 }

currentTemp = newReading  -- Desugars to: currentTemp = Fahrenheit(newReading)
```

**Rule**: The semantic pass only applies **one hop** of implicit casting. Multi-hop conversions (e.g., `Seconds` → `Minutes` → `Hours`) must be written explicitly by the programmer.
```

```luc
-- call site: TypeName(expr) dispatches to the matching from declaration
let boiling Celsius    = Celsius    { value = 100.0 }
let hot     Fahrenheit = Fahrenheit(boiling)         -- calls export from Fahrenheit (c Celsius)

let abs  Kelvin     = Kelvin     { value = 373.15 }
let absF Fahrenheit = Fahrenheit(abs)                -- calls export from Fahrenheit (k Kelvin)

-- works as a pipeline step
boiling -> Fahrenheit -> io.printl
```


## Pipeline Operator `->`

`->` executes a chain left-to-right at runtime. The chain has two distinct
roles: a **seed** and one or more **steps**.

```
pipeline_expr   := pipeline_seed { '->' pipeline_step }

pipeline_seed   := expr
                   -- any expression: variable, literal, function call result,
                   -- arithmetic result — produces the initial value for the chain

pipeline_step   := IDENTIFIER                          -- function by name
                 | IDENTIFIER ':' IDENTIFIER           -- impl method reference: Vec2:normalize
                 | IDENTIFIER '.' IDENTIFIER           -- data field of function type (non-nullable only)
                 | IDENTIFIER '(' arg_list ')' '!'     -- argument pack: extra args, upstream injected first
                 | anon_func                           -- inline anonymous function
                   -- must be callable — verified by the semantic pass
                   -- a bare value (literal, non-function variable) is not a valid step
```

The seed is evaluated first, producing a value. Each step must be a function.
`->` is the **only** operator that performs a function call in a pipeline. It
collects the upstream value and any extra arguments, then calls the function.

**How `->` resolves each step form:**

| Step form | What `->` does | Nullability |
|---|---|---|
| `fn` | calls `fn(upstream)` | must be non-nullable |
| `Type:method` | calls `method(upstream)` — method from Type's impl | always safe — behavior is never nullable |
| `struct.field` | calls the function stored in the data field | field must be non-nullable function type |
| `fn(args)!` | calls `fn(upstream, args...)` — upstream injected as first arg | must be non-nullable |
| `anon_func` | calls the anonymous function with upstream as argument | always safe |

### Data fields as pipeline steps

A struct data field of function type can be used as a pipeline step **only if
its type is not nullable**. The semantic pass checks the field's declared type
before allowing it in a step position.

```luc
struct Processor {
    transform  (v Vec2) Vec2          -- non-nullable: safe to use directly
    onComplete (() )?          = nil  -- nullable: must guard before use
}

let p Processor = Processor { transform = Vec2:normalize }

-- non-nullable field: safe as a pipeline step
v -> p.transform               -- calls p.transform(v)

-- nullable field: semantic error to use directly
v -> p.onComplete              -- ERROR: onComplete is nullable (() )?

-- nullable field: guard first, then use safely
if p.onComplete != nil {
    v -> p.onComplete          -- safe: type narrowed to () inside the if block
}
```

**Behavior members (`Type:method`) are always safe** — impl methods are never
nullable. The compiler can verify this statically without a runtime check.

### The `!` argument pack annotation

`fn(args)!` is **not a function call**. The `!` marks it as an argument pack —
the argument list is intentionally incomplete. The upstream value will be
injected as the first argument when `->` performs the actual call.

Without `!`, a call expression like `fn(args)` in a pipeline step position is
a **semantic error** — it looks like a complete call, and `->` has no declared
place to inject the upstream value.

```luc
let scale (v Vec2, factor float) Vec2 = { ... }   -- regular two-param function

-- WRONG: scale(2.0) looks like a complete call — Vec2 param is missing
v -> scale(2.0)      -- semantic error

-- CORRECT: scale(2.0)! is an argument pack — upstream Vec2 injected first
v -> scale(2.0)!     -- -> calls scale(v, 2.0)
```

**Semantic pass rule for `!`:**
1. See `fn(args)!` as a pipeline step
2. This is an argument pack — do not evaluate as a call
3. When `->` fires: assemble `fn(upstream, args...)` and call it
4. Param count check: `fn` must have exactly `1 + len(args)` parameters

**`!` is only valid as a pipeline step suffix** — using `fn(args)!` as a
standalone expression outside a pipeline is a semantic error.

### `->` vs `+>` — surface similarity, fundamental difference

Both operators chain functions left-to-right, which can look identical on the
surface. The difference is **where control lives**:

| | `->` pipeline | `+>` composition |
|---|---|---|
| When it runs | runtime, inside a block | compile time, produces a function |
| Seed | required — expression that starts the chain | none — pure function wiring |
| Steps | functions only, checked at semantic time | functions only, checked at compile time |
| Control flow | full access — `if`, `return`, loops | none — pure wiring |
| Surrounding state | can read/write any variable in scope | no scope, no state |
| Type strictness | relaxed — step may ignore previous result | strict — output type must match input type |
| Result | executes the chain now | a new function to call later |

`->` lives inside an imperative block. Because it is just sequential execution,
you can branch before it, return early to skip remaining steps, or gate it on
runtime state — anything you can do in a block applies:

```luc
const maxCalls int = 5
let callCount int = 0

let call (args any) = {
    if callCount >= maxCalls {
        return              -- skips f1(args) -> f2 entirely
    }
    callCount += 1
    f1(args) -> f2
}
```

`+>` has none of this — it is a compile-time wiring operation. The composed
function `f1 +> f2` is fully determined before any data exists. There is no
block, no surrounding state, no branching possible.

The surface-similar forms compared directly:

```luc
-- These look equivalent but are fundamentally different:

let f (args any) = { f1(args) -> f2 }
-- -> : f1(args) is the seed (a call expression), f2 is a step.
--      f1 runs now, its result is passed to f2.
--      'f' is a wrapper — controls when and whether the chain runs.

let f = f1 +> f2
-- +> : no execution. f is a new function whose input matches f1's
--      and whose output matches f2's. f(args) runs the chain later.
--      no opportunity to intercept, branch, or stop mid-chain.
```

### Return value passing — relaxed model

Each step receives the previous result as its first argument **only if** it
declares a compatible parameter. If the step declares no parameters, the
previous result is silently discarded.

```luc
-- step ignores previous result
let f1 (args ...any) int string = { return 42, "ok" }
let f2 () = { io.printl("step two") }

let chain (args ...any) = { f1(args) -> f2 }
-- f1 runs, returns (42, "ok"), f2 runs with no args — result discarded
```

This allows pipelines to act as **instruction chains** where not every step
needs to consume data from the previous one.

### Error propagation in pipelines

When the `error` library is used, the pipeline short-circuits on `Error`. If
any step returns an `Error` and the next step's parameter type does not accept
`Error`, the step is skipped and the `Error` is passed forward automatically
to the end of the chain — where `??` or `.catch` can handle it.

```luc
-- if readFile returns Error, parseConfig and validateConfig are skipped
-- the Error flows to ?? which provides the fallback
let cfg Config = readFile("app.cfg")
    -> parseConfig
    -> validateConfig
    ?? Config {}
```

See `LUC_ERROR.md` for full error handling documentation.

### Type conversion in pipelines

Primitive type names (`int`, `float`, `string`, `bool`, etc.) are callable as
conversion functions. This makes them valid pipeline steps:

```luc
42 -> float -> sqrt        -- int literal → float → sqrt
x  -> string -> io.printl  -- variable x converted to string, then printed
```

Unsafe bit reinterpret uses `*` prefix — only valid in `@extern` declaration contexts. For general-purpose bit reinterpretation in expression position, prefer `@bitcast(T, x)`:

```luc
bits -> *float             -- reinterpret uint32 bits as float32, no computation
                           -- (valid only inside an @extern context)

-- Preferred modern form anywhere:
let asFloat float = @bitcast(float, bits)
```

### User-defined type conversion

User types define conversions using top-level `from` declarations — see the
**From Declaration** section for the full grammar and rules.

`from` is a reserved keyword with special compiler meaning. When the compiler
sees `TargetType(expr)`, it looks for a top-level `from` declaration whose
target type and source parameter type match. Multiple `from` declarations are
allowed for the same target, each accepting a different source type.

```luc
-- call site: TypeName(expr) dispatches to the matching from declaration
let boiling Celsius    = Celsius    { value = 100.0 }
let hot     Fahrenheit = Fahrenheit(boiling)       -- dispatches to from Fahrenheit (c Celsius)

let abs     Kelvin     = Kelvin     { value = 373.15 }
let absF    Fahrenheit = Fahrenheit(abs)            -- dispatches to from Fahrenheit (k Kelvin)

-- works as a pipeline step
boiling -> Fahrenheit -> io.printl
```

Unsafe bit reinterpret of user types uses `*` prefix — only valid in `@extern` declaration contexts. Prefer `@bitcast(T, x)` in general expression position:

```luc
rawBytes -> *GpuVertex    -- reinterpret raw memory as GpuVertex, no conversion
                          -- (valid only inside an @extern context)
```

### Usage forms

```luc
-- Seed is a literal
42 -> float -> sqrt

-- Seed is a variable
x -> double -> string

-- Seed is a function call result
getUser(id) -> validate -> save

-- As a statement (result discarded)
f1(args) -> f2 -> f3

-- As an expression (capture result)
let result int = f1(args) -> f2 -> f3

-- Wrapped in a function (deferred / reusable)
let runPipeline (args int) int = { f1(args) -> f2 -> f3 }

-- With control flow inside the wrapper
let process (data any) = {
    if debug { data -> validate -> printStats }
    data -> transform -> upload
}

-- impl method reference as a step (TypeName:method)
let v Vector = Vector { x = 1.0  y = 1.0 }
v -> Vec2:normalize -> Vec2:length   -- normalize then get length

-- argument pack with ! — multi-param function, upstream injected first
let scale (v Vec2, factor float) Vec2 = { ... }
v -> scale(2.0)!                     -- -> calls scale(v, 2.0)

-- chaining both forms
v -> Vec2:normalize -> scale(2.0)!   -- normalize, then scale by 2
```

### Anonymous function step

The anonymous function must declare one parameter to receive the previous
result. It is a step — not a seed.

```luc
f1(args)
-> (result int) string {
    return if result > 0 ?? result -> string else "empty"
}
-> io.printl
```

## Composition Operator `+>`

`+>` wires functions together **without executing them**, producing a new function.
No input data is needed — `+>` operates purely on function types at compile time.

The compiler enforces that the **output type of the left** exactly matches the
**input type of the right**. A mismatch is a compile error.

> **See `->` vs `+>`** in the Pipeline Operator section above for a detailed
> comparison of when to use each. The short version: use `+>` when you want a
> reusable composed function with no runtime control needed; use `->` inside a
> block when you need branching, early return, or access to surrounding state.

```
compose_expr    := pipeline_expr { '+>' compose_operand }

compose_operand := IDENTIFIER                      -- named function
                 | IDENTIFIER ':' IDENTIFIER       -- impl method reference: Vec2:normalize
                 | IDENTIFIER '.' IDENTIFIER       -- data field of function type (non-nullable only)
                   -- left side is a pipeline expression (which may itself be a seed)
                   -- right side must be a concrete non-nullable callable
                   -- generic functions must be explicitly instantiated before composing
```

### Data fields and nullability in `+>`

A struct data field of function type can be used as a `+>` operand **only if
its type is not nullable**. Since `+>` operates at compile time, the semantic
pass must be able to verify the type statically.

```luc
struct Processor {
    transform  (v Vec2) Vec2          -- non-nullable: valid in +>
    onComplete (() )?          = nil  -- nullable: cannot use in +>
}

let p Processor = Processor { transform = Vec2:normalize }

-- non-nullable data field: safe in composition
let pipeline = p.transform +> render   -- valid

-- nullable data field: compile error
let pipeline = p.onComplete +> render  -- ERROR: onComplete is nullable

-- nullable field: guard first, then compose inside the safe block
if p.onComplete != nil {
    -- type narrowed inside the block — still cannot use in +>
    -- +> is compile-time; runtime guards don't satisfy it
    -- use -> inside the block instead:
    data -> p.onComplete
}
```

**Behavior members (`Type:method`) are always valid** in `+>` — impl methods
are never nullable, so the compiler can guarantee safety statically.

### Generic functions and `+>`

`+>` requires both sides to be **concrete** at compile time. If either side is a
generic function, the type parameters must be explicitly provided — inference
across a composition chain is not supported.

```luc
-- define concrete functions first, then compose
let doubleInt   (x int)    int    = { return x * 2 }
let intToString (x int)    string = { return string(x) }

let process = doubleInt +> intToString   -- valid: both sides are concrete

-- generic: must instantiate explicitly before composing
let process<T : Numeric> = double<T> +> stringify<T>
                           -- T is explicit — no inference needed
```

If you need runtime control or want to avoid explicit instantiation, use `->` in
a wrapper block instead:

```luc
let process<T : Numeric> (x T) string = { double(x) -> stringify }
```

### Usage forms

```luc
-- Compose two or more functions into one
let process = validate +> transform +> render

-- The result is a new function — call it like any other
process(data)

-- Feed a composed function into a -> pipeline
f1(args) -> preprocess +> transform -> output
```

### Compiler validation

```luc
let f (a int) string  = { ... }
let g (s string) bool = { ... }

let h   = f +> g    -- valid:   f returns string, g takes string
let bad = g +> f    -- ERROR:   g returns bool, f takes string — type mismatch
```

## Currying

Luc uses **chained parameter groups** as its single currying model. A function
with multiple parameter groups is syntactic sugar for a function that returns
another function. Each `()` group is one call — passing arguments to the first
group returns a new function expecting the next group, and so on until all
groups are satisfied and the final return value is produced.

The syntax `(a int) (b int) int` is the **sugar form** — it reads as "takes
`a int`, then takes `b int`, then returns `int`". The compiler treats this as
a function taking `a int` and returning a function `(b int) int`. You never
have to write that nested form yourself — the chained group notation is always
the intended syntax.

```luc
let add (a int) (b int) int = { return a + b }
--      ───────  ───────  ───
--      group 1  group 2  final return type

add(10)        -- supply group 1 → returns a new function: (b int) int
add(10)(5)     -- supply group 2 → returns the final value: 15
```

A single-group function is a normal function — no currying involved. Currying
only activates when two or more groups are declared.

### Grammar

```
func_decl       := decl_keyword IDENTIFIER [ generic_params ]
                   param_group { param_group } [ return_type ]
                   '=' func_body

param_group     := '(' [ param_list ] ')'
                   -- one group  = normal function
                   -- two+ groups = curried function (each group is one partial call)
```

### Usage forms

```luc
-- Two-group: classic curry
let add (a int) (b int) int = { return a + b }

add(10)            -- partial: returns (b int) int
add(10)(5)         -- full:    15

-- Three-group: each group narrows the function further
let clamp (min int) (max int) (value int) int = {
    if value < min { return min }
    if value > max { return max }
    return value
}

let clampPositive = clamp(0)       -- (max int) (value int) int
let clamp0to100   = clamp(0)(100)  -- (value int) int
clamp0to100(42)                    -- 42
clamp0to100(150)                   -- 100

-- Chains naturally with -> and +>
let addTen = add(10)               -- (b int) int
42 -> addTen -> string             -- "52"

let process = add(10) +> string    -- (b int) string
process(5)                         -- "15"
```

### What the compiler sees

The chained group syntax is sugar. The compiler desugars it into a function
that explicitly returns an anonymous function. This is shown here for
understanding — you always write the sugar form, never the desugared form:

```luc
-- what you write (sugar):
let add (a int) (b int) int = { return a + b }

-- what the compiler produces internally (do not write this):
let add (a int) (b int) int = { return (b int) int { return a + b } }
--                                      ─────────────────────────────
--                                      anonymous function capturing 'a'
```

The desugared body returns an anonymous function `(b int) int { return a + b }`
that closes over `a`. When you call `add(10)`, the compiler binds `a = 10` and
returns that anonymous function. When you then call it with `(5)`, `b = 5` and
`a + b = 15` is computed. The sugar hides all of this — you declare intent,
the compiler handles the nesting.

### Closures and partial application

Every partial application of a curried function produces a **closure** — the
returned function captures the already-supplied arguments from the outer call.
This happens automatically with the sugar form; you do not need to write the
explicit anonymous function to get closure behaviour.

```luc
let add (a int) (b int) int = { return a + b }

let addTen = add(10)
-- addTen is a closure: it captures a = 10
-- calling addTen(5) returns 15, calling addTen(20) returns 30
-- 'a' is fixed in the closure, 'b' varies on each call
```

The explicit (desugared) form can be used when you want to make the capture
intent visible, or when you are closing over a **local variable** rather than
a parameter — in which case the chained group syntax does not apply and you
write the anonymous function directly:

```luc
let multiplier int = 3

-- closing over a local variable — cannot use chained groups for this
-- must write the anonymous function explicitly
let scale = (x int) int { return x * multiplier }

scale(5)    -- 15  (multiplier = 3 captured from scope)
```

The rule of thumb: use chained groups when the captured values are the
function's own parameters. Use an explicit anonymous function when you need
to capture something from the surrounding block scope.

## Expressions

```
expr            := assign_expr

assign_expr     := compose_expr [ assign_op assign_expr ]

assign_op       := '=' | '+=' | '-=' | '*=' | '/=' | '^=' | '%='

compose_expr    := pipeline_expr { '+>' compose_operand }
                   -- see Composition Operator section for full usage forms

pipeline_expr   := pipeline_seed { '->' pipeline_step }
                   -- see Pipeline Operator section for full usage forms

pipeline_seed   := logical_expr

pipeline_step   := IDENTIFIER                              -- function by name
                 | IDENTIFIER ':' IDENTIFIER               -- impl method: Vec2:normalize
                 | IDENTIFIER '.' IDENTIFIER               -- data field of non-nullable function type
                 | IDENTIFIER '(' arg_list ')' '!'         -- argument pack: fn(args)!
                 | anon_func

logical_expr    := compare_expr { ( 'and' | 'or' ) compare_expr }
                   -- 'and' and 'or' are SHORT CIRCUIT operators:
                   --   and: if left is false, right is NOT evaluated, result is false
                   --   or:  if left is true,  right is NOT evaluated, result is true
                   -- both operands must be bool or nullable type
                   -- using a non-bool, non-nullable operand is a semantic error

compare_expr    := bitwise_expr [ compare_op bitwise_expr ]
                 | bitwise_expr 'is' type          -- type check: x is int, x is Circle
                                                   -- produces bool, narrows type in enclosing block

compare_op      := '=='    -- value equality   (see Comparison Rules section)
                 | '!='    -- value inequality
                 | '<'
                 | '>'
                 | '<='
                 | '>='
                 | '==='   -- reference equality: checks same memory address
                           -- valid on &T, structs, and nullable types only

bitwise_expr    := shift_expr   { ( '&&' | '||' | '~^' | '~' ) shift_expr }
                   -- '&&'  bitwise AND  (integer types only)
                   -- '||'  bitwise OR   (integer types only)
                   -- '~^'  bitwise XOR  (integer types only)
                   -- '~'   bitwise NOT  (unary, integer types only)
                   -- NOTE: '&' in expression position is the unary reference operator
                   --       '|' in type position is the union type separator
                   --       '&&' and '||' are used here to avoid ambiguity

shift_expr      := add_expr     { ( '<<' | '>>' ) add_expr }

add_expr        := mul_expr     { ( '+' | '-' ) mul_expr }

mul_expr        := pow_expr     { ( '*' | '/' | '%' ) pow_expr }

pow_expr        := unary_expr   [ '^' pow_expr ]      -- right-associative

unary_expr      := ( 'not' | '-' | '~' | '&' ) unary_expr
                 | postfix_expr

postfix_expr    := primary_expr { postfix_op }

postfix_op      := '.' IDENTIFIER                     -- data field access: v.x
                 | ':' IDENTIFIER                     -- impl method reference: Vec2:normalize
                 | '.?' IDENTIFIER                    -- nullable chain
                 | '??' expr                          -- nil fallback (terminates .? chain)
                 | '[' expr ']'                       -- index access: nums[2]
                 | '[' expr '..' expr ']'             -- slice inclusive end: nums[1..3]  = elements 1,2,3
                 | '[' expr '..<' expr ']'            -- slice exclusive end: nums[1..<3] = elements 1,2
                 | '(' [ arg_list ] ')'               -- call
                 | '(' [ arg_list ] ')' '!'           -- argument pack (pipeline only)
                 | generic_args '(' [ arg_list ] ')'  -- generic call

primary_expr    := literal
                 | IDENTIFIER
                 | struct_literal                         -- Vec2 { x = 1.0  y = 2.0 }
                 | '(' expr ')'
                 | anon_func
                 | match_expr                             -- match is expression-oriented; produces a value
                 | if_expr                                -- inline if expression: if cond ?? thenExpr else elseExpr
                 | array_literal
                 | 'nil'
                 | 'true' | 'false'
                 | await_expr
                 | range_expr

await_expr      := 'await' expr

range_expr      := expr range_op expr [ '..' expr ]
                   -- start range_op end [ .. step ]
                   -- range_op controls whether the end is included or excluded
                   -- step is always optional; when present it must follow '..' regardless of range_op
                   -- used in for loops, match patterns, switch cases, and slice indexing
                   -- see Arrays section for slice range rules and constraints

range_op        := '..'                -- inclusive end:  start ..  end  → [start, end]
                 | '..<'               -- exclusive end:  start ..< end  → [start, end)
```

### Argument List

```
arg_list        := expr { [','] expr }
```

### Array Literal

```
array_literal   := '[' [ expr { [','] expr } ] ']'
                   -- the compiler infers the array kind from context:
                   -- let nums [3]int  = [1, 2, 3]    -- fixed
                   -- let nums []int   = [1, 2, 3]    -- slice (view of a literal)
                   -- let nums [*]int  = [1, 2, 3]    -- dynamic
```

## Arrays

Luc has three distinct array kinds. Each has a clear memory model and a defined
set of built-in operations.

### Three Array Kinds

| Kind | Syntax | Memory | Growable | Use case |
|---|---|---|---|---|
| Fixed | `[N]T` | stack/inline, compile-time size | ❌ | Vulkan buffers, GPU data, known-size |
| Slice | `[]T` | view (pointer + length + cap), no ownership | ❌ | function params, iteration, pipeline |
| Dynamic | `[*]T` | heap-owned, runtime size | ✅ | growing collections, general purpose |

> **NOTE** 
> Array elements are not nullable by default, if you tried to assign a nil value to an array element, it would result in a compile-time error. To allow nullable index use `?`, ex: let nums [*]int? = [1, nil, 3]

```luc
-- Fixed array — size is part of the type, allocated inline
let rgba  [4]float  = [1.0, 0.5, 0.0, 1.0]
let mat4  [16]float = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0] -- the array element is not nullable therefore the element must be initialized.

-- Slice — a view into an existing array, no allocation
let nums  [*]int    = [10, 20, 30, 40, 50]
let view  []int     = nums[1..3]    -- view of elements 10, 20, 30 at indices 1,2,3

-- Dynamic array — heap-owned, can grow
let items [*]string = ["hello", "world"]
items.push("luc")
```

> **NOTE** 
> Out of bounds access on fixed or sliced array will result in a runtime panic. for dynamic array it will return a nil value.

### Slice Semantics

A slice is a **fat pointer** — internally `{ ptr, len, cap }`. It points into an
existing array (fixed or dynamic) and sees a contiguous subrange of its elements.
Slices do not own memory and cannot grow.

```
slice_expr      := expr '[' expr '..'  expr ']'    -- inclusive end: elements start..end
                 | expr '[' expr '..<' expr ']'    -- exclusive end: elements start..(end-1)
                   -- start must be >= 0
                   -- end must be >= start  (for '..')  or  > start  (for '..<')
                   -- both must be within array bounds
                   -- negative indices are not allowed
```

#### Slice range rules

Two range operators are available for slicing. The end bound interpretation
differs between them:

| Operator | End bound | `nums[1..3]` / `nums[1..<3]` on a 5-element array |
|---|---|---|
| `..` | **inclusive** — end element is included | `nums[1..3]` → elements 1, 2, 3 (3 elements) |
| `..<` | **exclusive** — end element is excluded | `nums[1..<3]` → elements 1, 2 (2 elements) |

General validity rules (apply to both operators):

| Expression | Valid? | Meaning |
|---|---|---|
| `nums[1..3]` | ✅ | elements at index 1, 2, 3 — inclusive end |
| `nums[1..<3]` | ✅ | elements at index 1, 2 — exclusive end |
| `nums[0..0]` | ✅ | single element at index 0 |
| `nums[0..<1]` | ✅ | single element at index 0 |
| `nums[0..<0]` | ❌ | semantic error: exclusive end must be > start |
| `nums[5..1]` | ❌ | semantic error: start > end |
| `nums[-1..2]` | ❌ | semantic error: negative index |
| `nums[0..99]` on len-5 array | ❌ | runtime panic: out of bounds |

The inclusive form `..` — length = `end - start + 1`.
The exclusive form `..<` — length = `end - start`.

```luc
let nums [*]int = [10, 20, 30, 40, 50]   -- indices 0..4

let s  []int = nums[1..3]    -- [20, 30, 40]  (3 elements — inclusive)
let se []int = nums[1..<3]   -- [20, 30]       (2 elements — exclusive)
s.len()                      -- 3
se.len()                     -- 2

-- Slices share memory with the original
s[0] = 99                    -- nums[1] is now 99
```

### Index Access

```
index_expr      := expr '[' expr ']'
                   -- expr must be a non-negative integer
                   -- must be < array.len()
                   -- out of bounds: runtime panic (non-recoverable system error)
```

```luc
nums[0]         -- first element
nums[2]         -- third element
nums[nums.len() - 1]   -- last element (use .last() for cleaner code)
nums[-1]        -- ERROR: negative index not allowed
```

### Index Assignment

Writing to an array element uses the same `assign_stmt` form. The LHS is a
`postfix_expr` with an index op — the semantic pass verifies the array is held
by a `let` variable.

```luc
let nums [*]int = [1, 2, 3]
nums[0] = 99          -- valid: nums is let, index is in bounds
nums[1] += 5          -- compound assignment on array element: valid

const frozen [3]int = [1, 2, 3]
frozen[0] = 99        -- ERROR: const variable is not reassignable
```

### Built-in Methods

All built-in methods are available without any import. Extended operations
(sort, filter, map, reduce) are provided by `use array`.

#### All array kinds — `[N]T`, `[]T`, `[*]T`

| Method | Returns | Description |
|---|---|---|
| `.len()` | `int` | number of elements |
| `.isEmpty()` | `bool` | true if `len() == 0` |
| `[i]` | `T` | element at index (panics if out of bounds) |
| `[i..j]` | `[]T` | inclusive slice — elements i through j |

#### Slice and dynamic — `[]T`, `[*]T`

| Method | Returns | Description |
|---|---|---|
| `.cap()` | `int` | allocated capacity |
| `.first()` | `T` | first element |
| `.last()` | `T` | last element |

#### Dynamic only — `[*]T`

| Method | Returns | Description |
|---|---|---|
| `.push(value T)` | — | append element to end |
| `.pop()` | `T` | remove and return last element |
| `.insert(i int, v T)` | — | insert at index, shift elements right |
| `.remove(i int)` | `T` | remove at index, return it (panics if out of bounds) |
| `.clear()` | — | remove all elements, keep capacity |
| `.reserve(n int)` | — | pre-allocate capacity hint for `n` total elements |

#### Concatenation operator

`+` is defined on `[]T` and `[*]T` — produces a new array containing all
elements of both operands:

```luc
let a [*]int = [1, 2, 3]
let b [*]int = [4, 5, 6]
let c [*]int = a + b    -- [1, 2, 3, 4, 5, 6]
```

### Array as Data Structure Foundation

The three array kinds are the foundation for all standard library data
structures. Higher-level structures are built on top and provided as libraries:

| Library | Built on | Structure |
|---|---|---|
| `use array` | `[]T`, `[*]T` | sort, filter, map, reduce, find |
| `use stack` | `[*]T` | push/pop LIFO wrapper |
| `use queue` | `[*]T` | ring buffer FIFO |
| `use hashmap` | `[*]T` + FFI | key-value hash table |
| `use set` | hashmap internals | unique value collection |
| `use linkedlist` | `&T` references | pointer-based node chain |
| `use tree` | `&T` references | pointer-based tree node |

`linkedlist` and `tree` are pointer-based — they use `&T` references rather
than arrays. All others use `[*]T` as their underlying storage.

```luc
-- example: using array library
use array

let nums [*]int = [3, 1, 4, 1, 5, 9, 2, 6]

nums -> array.sort                              -- sort in place
nums -> array.filter((x int) bool { x > 3 })   -- new slice of elements > 3
nums -> array.map((x int) int { x * 2 })        -- new array with doubled values
```

## Statements

```
stmt            := var_decl
                 | func_decl
                 | assign_stmt
                 | if_stmt
                 | switch_stmt          -- statement: dispatches on value, runs stmts
                 | for_stmt
                 | while_stmt
                 | do_while_stmt
                 | return_stmt
                 | break_stmt
                 | continue_stmt
                 | parallel_stmt        -- parallel for (data) or parallel block (task)
                 | expr_stmt            -- any expression used as a statement; match_expr and if_expr are valid here

block           := '{' { stmt } '}'

expr_stmt       := expr
assign_stmt     := expr assign_op expr
                   -- plain '=' accepts any expr on the RHS
                   -- compound operators (+=, -=, etc.) desugar to x = x op expr
                   -- type validity is checked after desugaring — see Compound Assignment section
```

## Compound Assignment

Compound assignment operators are shorthand — `x += expr` desugars to
`x = x + expr`. The compiler type-checks the desugared form using the same
rules as a regular `+` expression. If `+` is valid between the LHS type and
the RHS type, the compound assignment is valid.

```
compound_assign := IDENTIFIER compound_op expr
                 | postfix_expr compound_op expr   -- e.g. obj.field += 1

compound_op     := '+=' | '-=' | '*=' | '/=' | '^=' | '%='
```

### Desugaring

```luc
x += expr     -- desugars to:  x = x + expr
x -= expr     -- desugars to:  x = x - expr
x *= expr     -- desugars to:  x = x * expr
x /= expr     -- desugars to:  x = x / expr
x ^= expr     -- desugars to:  x = x ^ expr  (power)
x %= expr     -- desugars to:  x = x % expr  (modulo)
```

The RHS `expr` can be anything the operator accepts — a literal, a variable,
a field access, an arithmetic expression, or a function call that returns a
compatible type. The semantic pass validates after desugaring using the normal
type rules for the operator.

```luc
-- numeric
x       += 1
x       += getCount()       -- valid if getCount() returns a numeric type
hp      -= getDamage()
scale   *= getFactor()
x       ^= 2                -- x = x ^ 2
index   %= length           -- x = x % length

-- string  (+= is valid because + is defined on string)
name    += " world"
label   += buildSuffix()    -- valid if buildSuffix() returns string

-- field access on LHS
obj.health -= damage        -- valid if health is a let-mutable numeric field
obj.name   += " Jr"         -- valid if name is a let-mutable string field
```

### LHS rules

- The left-hand side must resolve to a `let` variable or a field of a struct
  held by a `let` variable — `const` is not reassignable, so compound
  assignment on it is a semantic error
- The operator must be defined for the LHS type — `-=` on a `string` is a
  semantic error because `-` is not defined on strings

## Comparison Rules

Luc has two equality operators and strict rules about which types they apply to.

### `==` — Value Equality

`==` compares **values**. It never compares types — that is the job of `is`.

```
== and != are defined for:
  primitives     int, float, double, bool, char, string
  enum variants  Direction.North == Direction.South
  nullable types int? == int?  |  int? == nil  |  int? == 5
```

```
== and != are NOT defined for (semantic error):
  struct types   Vec2 == Vec2       ERROR: implement Equatable<T> instead
  function types (x int) int == ... ERROR: function bodies are incomparable
  array types    [*]int == [*]int   ERROR: use collection library comparison
```

**Nullable comparisons with `==`:**

```luc
let x int? = 5
let y int? = nil

x == 5    -- valid: compares unwrapped value, true
x == nil  -- valid: nil check, false
x != nil  -- valid: nil check, true
y == nil  -- valid: nil check, true
x == y    -- valid: 5 != nil, false

-- is checks the TYPE, not the value
x is int  -- false: int? is NOT the same type as int
          -- nullable and non-nullable are distinct types
```

**Struct equality requires explicit trait implementation:**

```luc
-- this is a semantic error
let a Vec2 = Vec2 { x = 1.0  y = 1.0 }
let b Vec2 = Vec2 { x = 1.0  y = 1.0 }
if a == b { ... }   -- ERROR: struct type Vec2 does not support ==
                    -- implement Equatable<Vec2> to enable equality

-- correct approach: implement the trait
pub impl Vec2 : Equatable<Vec2> {
    equals (other Vec2) bool = {
        return x == other.x and y == other.y
    }
}

-- then call equals explicitly — == is still not defined
-- the Equatable trait provides equals(), not ==
if a:equals(b) { ... }   -- valid
```

### `===` — Reference Equality

`===` checks whether two expressions refer to the **same memory location**. It
does not compare values — it compares addresses.

```
=== is defined for:
  &T reference types        two references pointing to the same object
  struct types              checks if two struct values occupy the same address
  nullable reference types  &T? === &T?
```

```luc
let a Vec2 = Vec2 { x = 1.0  y = 1.0 }
let b Vec2 = a           -- b is a copy

if a === b { ... }   -- false: a and b are different memory locations
                     -- b is a copy, not the same object

let ref1 &Vec2 = a
let ref2 &Vec2 = a

if ref1 === ref2 { ... }   -- true: both reference the same a
```

### `is` — Type Identity

`is` checks the **type** of a value, not its content. It never compares values.
`is` also **narrows the type** inside the enclosing block (statement form only).

```
== checks VALUE   — are these the same value?
is checks TYPE    — is this value of this type?

-- These are completely different questions:
x == true     -- is x's value equal to true?   (x must already be bool)
x is bool     -- is x's type bool?             (x could be any)

-- Nullable types and their base types are distinct:
let x int? = 5
x is int    -- false: int? is NOT int, nullable ≠ non-nullable
x is int?   -- true:  x is declared as int?
x == 5      -- true:  value comparison, unwraps nullable automatically
```

### Chained Comparisons — Not Allowed

Chaining comparison operators directly is a semantic error. Use `and` explicitly:

```luc
-- WRONG: chained comparison
if 0 < x < 10 { ... }     -- ERROR: parsed as (0 < x) < 10
                            -- (0 < x) produces bool, bool < int is a type error

-- CORRECT: use and
if 0 < x and x < 10 { ... }   -- valid
```

---

## Logical Operators

### `and` / `or` — Short Circuit Boolean Logic

`and` and `or` operate on `bool` and nullable types. They **short circuit** —
the right operand is only evaluated if necessary.

```
and: if left is false  → result is false,  right is NOT evaluated
or:  if left is true   → result is true,   right is NOT evaluated
```

```luc
-- short circuit: validate() only called if getUser() != nil
if getUser() != nil and validate(getUser()) {
    ...
}

-- short circuit: expensiveLoad() only called if cache:has(key) is false
if cache:has(key) or expensiveLoad(key) {
    ...
}

-- both operands must be bool or nullable
-- using a non-bool, non-nullable type is a semantic error
if 1 and true { ... }   -- ERROR: left operand is int, not bool
```

### `not` — Logical Negation

`not` operates on `bool` and nullable types.

```luc
-- on bool: standard logical negation
if not isValid { ... }

-- on nullable: nil is treated as false, any non-nil value is treated as true
let x int? = nil
if not x { ... }   -- true: x is nil, treated as false, not flips to true

let y int? = 5
if not y { ... }   -- false: y is non-nil, treated as true, not flips to false

-- not on any other type is a semantic error
let n int = 5
if not n { ... }   -- ERROR: n is int, not bool or nullable
                   -- write: if n == 0 { ... }  instead
```

---

## Bitwise Operators

Bitwise operators work on **integer types only**. Using them on non-integer types
is a semantic error.

| Operator | Name | Example | Notes |
|---|---|---|---|
| `&&` | bitwise AND | `a && b` | integer types only |
| `\|\|` | bitwise OR  | `a \|\| b` | integer types only |
| `~^` | bitwise XOR | `a ~^ b` | integer types only |
| `~` | bitwise NOT | `~a` | unary, integer types only |
| `<<` | left shift  | `a << n` | integer types only |
| `>>` | right shift | `a >> n` | integer types only |

**Why `&&` and `||` instead of `&` and `|`:**

`&` is the reference operator (`&T`, `&x`) and `|` is the union type separator
(`int | string`). Using them as bitwise operators would create ambiguity in both
type position and expression position. `&&` and `||` are unambiguous in all
contexts.

```luc
-- bitwise operations
let flags  uint32 = 0xFF00
let mask   uint32 = 0x0F0F
let result uint32 = flags && mask    -- 0x0F00  bitwise AND
let merged uint32 = flags || mask    -- 0xFF0F  bitwise OR
let flipped uint32 = flags ~^ mask   -- 0xF00F  bitwise XOR
let inverted uint32 = ~flags         -- bitwise NOT

-- shift operations
let shifted uint32 = 1 << 4    -- 16
let halved  uint32 = 256 >> 2  -- 64

-- common pattern: flag checking
const VISIBLE uint32  = 0x01
const ACTIVE  uint32  = 0x02
const DIRTY   uint32  = 0x04

let entityFlags uint32 = VISIBLE || ACTIVE

if entityFlags && VISIBLE != 0 { ... }   -- check if VISIBLE flag is set
if entityFlags && DIRTY   == 0 { ... }   -- check if DIRTY flag is NOT set
```

---

## Type Checking

Luc provides two mechanisms for checking the type of a value at runtime:
the `is` operator for boolean checks, and type patterns in `match` for
dispatch. Both trigger **type narrowing** — the compiler knows the concrete
type inside the branch and gives full access to its methods and fields.

### `is` operator

```
is_expr         := expr 'is' type
                   -- produces bool
                   -- narrows the type of expr inside the enclosing if_stmt block (statement form only)
                   -- the inline if_expr form does not introduce a narrowed scope
                   -- valid for: primitives, structs, union types, any, enum variants
                   -- IMPORTANT: nullable and non-nullable are distinct types
                   --   int? is int  → false  (int? is NOT int)
                   --   int? is int? → true
```

```luc
-- primitive check
if x is int {
    let doubled int = x * 2    -- x is known int here
}

-- user type check
if shape is Circle {
    let area float = shape.radius * shape.radius * 3.14159  -- shape is Circle here
}

-- any type check
let value any = getData()
if value is Vec2 {
    io.printl(string(value.x))       -- value is Vec2 here
}

-- enum variant check
if stage is ShaderStage.Fragment {
    bindFragmentShader()
}

-- nullable vs non-nullable: is checks the declared type exactly
let x int? = 5
if x is int  { ... }   -- false: int? is NOT int, types are distinct
if x is int? { ... }   -- true:  x is declared as int?
if x == 5   { ... }    -- true:  == compares value, unwraps nullable automatically
if x != nil { ... }    -- true:  nil check via ==, not via is
```

### Type patterns in `match`

A type pattern `v is Type` in a match arm binds the matched value to `v` with
the narrowed type, and takes the arm only if the value is of that type.

```luc
-- union type dispatch
type Shape = Circle | Rect | Triangle

let area (shape Shape) float = match shape {
    s is Circle   -> s.radius * s.radius * 3.14159
    s is Rect     -> s.width * s.height
    s is Triangle -> s.base * s.height / 2.0
    default       -> 0.0
}

-- any type dispatch
let describe (value any) string = match value {
    v is int    -> "int: "    + string(v)
    v is string -> "string: " + v
    v is bool   -> "bool: "   + string(v)
    v is Vec2   -> "vec2: "   + string(v.x) + ", " + string(v.y)
    default     -> "unknown"
}

-- enum variant dispatch
let label (dir Direction) string = match dir {
    Direction.North -> "north"
    Direction.South -> "south"
    Direction.East  -> "east"
    Direction.West  -> "west"
    default         -> "unknown"
}
```

### Type narrowing rules

After `if x is SomeType { ... }` (statement form), inside the block:
- `x` is treated as `SomeType` by the compiler
- All methods and fields of `SomeType` are accessible directly on `x`
- Outside the block, `x` reverts to its original declared type

In a `match` arm with `v is SomeType`:
- `v` is bound as `SomeType` for the duration of that arm's body
- The original matched expression is unchanged

> **Note** 
> Type narrowing applies to the statement form `if x is T { }` only. The inline expression form `if x is T ?? expr else expr` does not introduce a narrowed scope — use `match` with a type pattern for narrowed dispatch in expression context.

### If / Else — Statement Form

`if_stmt` is **statement-oriented** — it runs blocks for side effects and produces no value.
`else` is optional. The `else` branch can be another `if_stmt` for chaining.

```
if_stmt         := 'if' expr block [ 'else' ( if_stmt | block ) ]
```

```luc
-- no else
if score >= 90 {
    io.printl("A")
}

-- with else
if score >= 90 {
    io.printl("A")
} else {
    io.printl("F")
}

-- chained else if
if score >= 90 {
    io.printl("A")
} else if score >= 80 {
    io.printl("B")
} else {
    io.printl("F")
}
```

### If Expression — Inline Form

`if_expr` is **expression-oriented** — it produces a value and can be used anywhere
an expression is expected: variable assignment, function return, pipeline seed,
match arm body, or any other expression position.

The syntax uses `??` as the separator between the condition and the then-branch.
`else` is **required** — an inline if with no else branch is a syntax error.

```
if_expr         := 'if' expr '??' expr 'else' expr
                   -- condition ?? thenExpr else elseExpr
                   -- all three branches must produce compatible types
                   -- 'else' is required — no dangling if expression
                   -- right-associative: else binds to the nearest preceding if
                   -- elseExpr may itself be an if_expr for chaining
```

Chaining works naturally because `elseExpr` is any `expr`, and `if_expr` is an `expr`:

```luc
-- assignment
let grade string = if score >= 60 ?? "pass" else "fail"

-- chained (right-associative — else binds to nearest if)
let label string = if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive"

-- as function return
let sign (n int) string = {
    return if n >= 0 ?? "positive" else "negative"
}

-- as pipeline seed
(if useHigh ?? highValue else lowValue) -> process -> io.printl

-- in variable declaration — match and if are both valid expression initialisers
let a int = match n { 0 -> 1  default -> n * 2 }
let b int = if n > 0 ?? n else 0
```

> **Disambiguation from `??` (null fallback):** `??` after `if expr` is the
> inline-if separator. `??` after any other expression is the null-coalescing
> operator. The parser knows it is inside `if_expr` the moment it sees the `if`
> keyword, so the two uses are unambiguous.

> **Statement vs expression:** `if cond { block }` (block after condition) is
> always the statement form. `if cond ?? expr else expr` (`??` after condition)
> is always the expression form. The parser dispatches on the token immediately
> following the condition expression.

### Match (Pattern Matching — expression oriented)

`match` is an **expression** — it produces one or two values and can be
assigned or returned directly. Each arm maps a pattern to a comma-separated
list of one or two expressions (no blocks, no `return`). The first expression
is the **primary value**; the optional second expression is the **secondary
value**.

The `default` arm is **required** — match must be exhaustive and `default`
must be the last arm. The `_` wildcard pattern and `default` are distinct:
`_` is a pattern meaning "any value, discard it"; `default` is the required
fallback arm. Neither may appear before the last position or they would make
subsequent arms unreachable — the semantic pass enforces this.

```
match_expr      := 'match' expr '{' { match_arm } default_arm '}'

match_arm       := pattern_list [ guard ] '->' arm_body

default_arm     := 'default' '->' arm_body

-- An arm body is one or two comma-separated expressions — no blocks allowed.
-- The first expression is the primary value (always present).
-- The second expression is the secondary value (optional).
arm_body        := expr [ ',' expr ]

-- Multiple patterns per arm separated by comma.
-- The parser disambiguates pattern commas from the arm_body comma by position:
-- commas before '->' are pattern separators; the comma after '->' (if any)
-- separates primary from secondary value.
pattern_list    := pattern { ',' pattern }

-- Guard: bind pattern gives the name, if-expr filters it
-- The guard condition is a logical_expr — not a full expr — to prevent
-- ambiguity with the inline if_expr syntax (if cond ?? thenExpr else elseExpr).
-- A guard condition is always a plain boolean expression.
guard           := 'if' logical_expr

pattern         := literal                             -- value:    42, "ok", true
                 | range_expr                          -- range:    1..10 (inclusive), 1..<10 (exclusive)
                 | IDENTIFIER                          -- bind:     n  (captures matched value)
                 | IDENTIFIER 'is' type                -- type pattern: v is Circle  (bind + narrow)
                 | WILDCARD                            -- discard:  _
                 | struct_pattern                      -- shape:    Vec2 { x, y }

struct_pattern  := IDENTIFIER '{' { field_pattern } '}'
field_pattern   := IDENTIFIER [ ':' pattern ]          -- field: name  or  name: sub-pattern
```

### Secondary or more value rules

The secondary ore more value is **optional per arm**. The semantic pass enforces the
following rules based on whether arms supply it:

| Situation | Rule |
|---|---|
| No arm supplies a secondary or more value | The match produces one value. Assigning to two or more variables is a semantic error. |
| Every arm supplies a secondary or more value | The match produces two or more values. The second or more variable's type is non-nullable. unless the return value can be nil |
| Only some arms supply a secondary or more value | The match produces two or more values. The second or more variable **must** be typed as nullable (`T?`). Arms that omit the secondary or more value implicitly yield `nil` for it. |

The secondary or more values is silently discarded when the caller assigns only one
variable. Assigning to two or more variables when no arm produces a secondary or more value
is a semantic error.

### Pattern rules

A **bind pattern** is any bare `IDENTIFIER` in pattern position. It matches
any value and binds it to that name for use in the guard and arm body. The
name `n` in `n if n > 0` comes entirely from the pattern — `n` is declared
there, not from an outer scope.

A **guard** `if logical_expr` may only appear after a bind or wildcard pattern. It
gives a condition that must also be true for the arm to match. If the guard
is false the arm is skipped and matching continues to the next arm. The guard
condition is restricted to `logical_expr` (not a full `expr`) — inline `if_expr`
and pipeline expressions are not valid in guard position. This prevents ambiguity
between the guard `if` keyword and the `if cond ?? thenExpr else elseExpr` syntax.

**Arm bodies are expressions only** — no blocks, no `return`. Each arm body
is one or two comma-separated expressions. Multi-statement logic must be
extracted into a helper function called from the arm expression.

**Arm ordering matters** — arms are tested top to bottom. A bind pattern
without a guard matches everything and must come after more specific patterns.
`_` and `default` must be last.

**Array matching** — Luc does not have array destructure patterns. Use a
bind pattern with guards and method calls instead:

```luc
match items {
    arr if arr.len() == 0 -> "empty"
    arr if arr.len() == 1 -> "single: " + string(arr[0])
    arr                   -> "many: "   + string(arr.len())
    default               -> "unknown"
}
```

### Examples

```luc
-- Value pattern — single primary value
let label string = match status {
    200      -> "ok"
    404      -> "not found"
    500      -> "error"
    default  -> "unknown"
}

-- Multiple values per arm (comma-separated patterns)
let category string = match code {
    200, 201, 202 -> "success"
    400, 401, 403 -> "client error"
    500, 502, 503 -> "server error"
    default       -> "other"
}

-- Range pattern — inclusive (..) and exclusive (..<)
let severity string = match damage {
    0       -> "none"
    1..10   -> "light"       -- matches 1, 2, ..., 10 (inclusive)
    11..<50 -> "moderate"    -- matches 11, 12, ..., 49 (exclusive end)
    default -> "critical"
}

-- Bind pattern with guard
let label string = match score {
    n if n < 0   -> "invalid: " + string(n)
    n if n < 50  -> "fail: "    + string(n)
    n if n < 80  -> "pass: "    + string(n)
    n            -> "merit: "   + string(n)
    default      -> "unknown"
}

-- Wildcard
let label string = match value {
    0       -> "zero"
    _       -> "non-zero"
    default -> "fallback"
}

-- Struct destructuring
let desc string = match point {
    Vec2 { x: 0.0, y: 0.0 } -> "origin"
    Vec2 { x, y }            -> "at " + string(x) + ", " + string(y)
    default                  -> "unknown"
}

-- Nested struct destructuring
let desc string = match player {
    Player { health: 0 }                               -> "dead"
    Player { pos: Vec2 { x: 0.0, y: 0.0 }, health }   -> "at origin, hp: " + string(health)
    Player { pos, health }                             -> "alive, hp: "     + string(health)
    default                                            -> "unknown"
}

-- Combining multiple patterns and guard
let label string = match code {
    200, 201     -> "created or ok"
    n if n > 500 -> "critical: " + string(n)
    default      -> "other"
}

-- Secondary value — every arm supplies one (second variable is non-nullable)
let label string
let detail string
label, detail = match status {
    200     -> "ok",        "request succeeded"
    404     -> "not found", "resource missing"
    default -> "unknown",   "no detail available"
}

-- Secondary value — only some arms supply one (second variable must be nullable)
let label string
let detail string?
label, detail = match status {
    200     -> "ok",        "request succeeded"
    404     -> "not found"          -- detail is implicitly nil here
    default -> "unknown"            -- detail is implicitly nil here
}

-- Secondary value discarded — caller only captures the primary value
let label string = match status {
    200     -> "ok",        "request succeeded"   -- second value silently dropped
    404     -> "not found", "resource missing"
    default -> "unknown",   "no detail available"
}
```

### Switch (Value Dispatch — statement oriented)

`switch` is a **statement** — it dispatches on a value and runs a block of
statements. Multiple values and ranges are allowed per case. No fallthrough —
each case is isolated (unlike C).

```
switch_stmt     := 'switch' expr '{' { case_clause } [ default_clause ] '}'

case_clause     := 'case' case_value { ',' case_value } ':' { stmt }

case_value      := expr                                -- single value:  case 1:
                 | range_expr                          -- range:    case 0..10: (inclusive), case 0..<10: (exclusive)

default_clause  := 'default' ':' { stmt }
```

Examples:
```luc
-- Multiple values per case
switch code {
    case 200, 201, 202: { io.printl("success") }
    case 400, 401:      { io.printl("client error") }
    case 500:           { io.printl("server error") }
    default:            { io.printl("unknown") }
}

-- Range per case — inclusive (..) and exclusive (..<)
switch damage {
    case 0:         { io.printl("no damage") }
    case 1..10:     { io.printl("light") }      -- matches 1..10 inclusive
    case 11..<50:   { io.printl("moderate") }   -- matches 11..49 exclusive end
    default:        { io.printl("critical") }
}

-- Mixed values and ranges
switch key {
    case 0x41, 0x42:    { handleAB() }
    case 0x30..0x39:    { handleDigit() }
    default:            { handleOther() }
}
```

### For Loop

```
for_stmt        := 'for' IDENTIFIER [ type ] 'in' ( range_iter | expression ) block

range_iter      := expression range_op expression [ '..' expression ]
                   -- start range_op end [ .. step ]
                   -- range_op: '..' = inclusive end, '..<' = exclusive end
                   -- default step is 1 if omitted
                   -- NOTE: If type is omitted, it defaults to 'int' or 'float'
                   -- depending on whether boundaries are integer or float literals.
```

### While Loop

```
while_stmt      := 'while' expr block
```

### Do-While Loop

```
do_while_stmt   := 'do' block 'while' expr
```

### Return / Break / Continue

```
return_stmt     := 'return' [ expr ]

break_stmt      := 'break'

continue_stmt   := 'continue'
```

## Concurrency

### Async / Await

`async` marks a function as asynchronous — it may suspend at `await` points
and resume later without blocking the thread. An async function returns the
eventual value type; the compiler wraps the return in a future internally.
`await` suspends the current async function until the awaited future resolves,
then produces the actual value.

```
-- async modifier on an anonymous function body
anon_func       := 'async' '(' [ param_list ] ')' [ return_type ] block

-- await in expression position — only valid inside an async function
await_expr      := 'await' expr
```

### Rules

- `await` is only valid inside an `async` function body
- An async function may freely call non-async functions
- A non-async function calling an async function receives the future, not
  the resolved value — it cannot `await` it
- `await` is **not valid** inside a `parallel for` or `parallel` block body —
  parallel scopes are synchronous and CPU-bound; mixing async suspension
  into parallel iterations produces undefined ordering behaviour

### Examples

```luc
-- async function declaration
let fetch (url string) string = async (url string) string {
    let data string = await httpGet(url)
    return data
}

-- composing async and parallel: fetch first (async), then process (parallel)
let process (items []Item) []Result = async (items []Item) []Result {
    -- I/O phase: async and sequential
    let raw []RawData = await fetchAll(items)

    -- CPU phase: parallel and synchronous — no await inside
    parallel for item in raw {
        item.value = item.value -> transform -> normalize
    }

    return raw -> toResults
}
```

### Parallel

`parallel` has two forms depending on the use case:

**`parallel for`** — data-parallel iteration over a collection. Each element
is processed independently and simultaneously. This is the primary DOD form —
use it for vertex transforms, particle updates, entity processing, and any
operation where every element is independent.

**`parallel` block** — task-parallel execution of a fixed set of independent
operations simultaneously. Use it when you have a known number of independent
tasks to run concurrently (analogous to `Promise.all` or Go's goroutines).

```
parallel_stmt   := parallel_for
                 | parallel_block

parallel_for    := 'parallel' 'for' IDENTIFIER [ type ] 'in' ( range_iter | expression ) block
                   -- NOTE: If type is omitted, it defaults to 'int' or 'float'
                   -- depending on whether boundaries are integer or float literals.
                   -- IDENTIFIER is the iteration variable, local to the body
                   -- expr must be a collection type (array, slice)

parallel_block  := 'parallel' '{' { sub_block } '}'

sub_block       := block
                   -- each block runs as an independent concurrent task
```

### Parallel scope rules

The semantic pass enforces these rules inside any parallel body:

- **No shared mutable state** — writing to variables declared outside the
  parallel scope is a semantic error; reading outer variables is allowed
- **No `await`** — async suspension inside a parallel scope is a semantic error
- **No `return`** — a parallel body cannot return from the enclosing function
- **No `break` or `continue`** — control flow that escapes the iteration
  is not valid; use conditions inside the body instead
- **Iteration variable is local** — in `parallel for`, each iteration gets
  its own independent binding of the iteration variable

### Examples

```luc
-- parallel for: transform every vertex independently
parallel for vertex in mesh.vertices {
    vertex.pos    = vertex.pos    -> transform
    vertex.normal = vertex.normal -> normalize
    vertex.uv     = vertex.uv     -> wrapUV
}

-- parallel for with index via a range
parallel for i in 0..particles.len() {
    particles[i].pos += particles[i].velocity * dt
}

-- parallel block: run independent tasks simultaneously
parallel {
    { const textures = loadTextures(paths) }
    { const shaders  = compileShaders(sources) }
    { const meshes   = loadMeshes(files) }
}
-- all three complete before execution continues

-- combining async and parallel (fetch async, process parallel)
let build (paths []string) []Texture = async (paths []string) []Texture {
    let raw []RawImage = await loadAll(paths)     -- async I/O
    parallel for img in raw {                      -- parallel CPU
        img.data = img.data -> decompress -> toLinear
    }
    return raw -> toTextures
}
```

## Visibility & FFI Modifiers

### Visibility

Luc uses a two-layer visibility model. Each layer has a single dedicated
mechanism — they do not overlap.

```
-- Layer 1: file → package  (pub on declarations)
--
--   pub prefix on struct, type, func, var:
--     the declaration is accessible to other files in the same package
--     without pub, it is private to the file it is declared in
--
--   pub impl:
--     methods are callable by anyone holding the value (within or outside the package)
--
--   impl (bare):
--     methods are callable only within the package

pub_mod         := 'pub'
                   -- may prefix: struct_decl, trait_decl, type_decl,
                   --             func_decl, var_decl, impl_decl

-- Layer 2: package → world  (module manifest)
--
--   a module manifest (module PackageName { use ... }) declares which files
--   contribute to the package's external public API
--
--   without a manifest:  all pub declarations are externally visible (simple packages)
--   with a manifest:     only re-exported files are visible to external consumers
--
--   see Module System section for full grammar and examples
```

## `@` Compiler Directives

`@` is the **compiler authority** prefix. It marks anything that cannot be
expressed through ordinary procedural logic — FFI bindings, optimizer hints,
struct layout control, and compile-time intrinsic calls. If you see `@`, the
compiler is directly involved: either linking to an external symbol, guiding
the backend, or computing a value at compile time.

There are two syntactic positions for `@`:

| Position | Form | Purpose |
|---|---|---|
| **Attribute** | `@name` / `@name(args)` before a declaration | Attach metadata to a `let`, `const`, or `struct` |
| **Intrinsic call** | `@name(args)` in expression position | Compiler-builtin function call |

Attributes and intrinsic calls share the same `@name` syntax but are
distinguished by position. Attributes precede a declaration; intrinsic calls
appear where any other expression appears.

### Attributes

An attribute is a compile-time annotation placed on a declaration. Attributes
are processed during the semantic phase and stored in the symbol table.
Multiple attributes may appear before the same declaration, one per line or
stacked inline.

```
attribute       := '@' IDENTIFIER [ '(' attr_arg_list ')' ]

attr_arg_list   := attr_arg { ',' attr_arg }

attr_arg        := STRING_LITERAL      -- e.g. "malloc", "stdcall"
                 | INT_LITERAL         -- e.g. 8
                 | HEX_LITERAL         -- e.g. 0xFF
                 | 'true' | 'false'
                 | IDENTIFIER          -- type name: @sizeof(Vec2)
```

**Parameter restriction:** attribute arguments are intentionally limited to
compile-time literals and type identifiers. Runtime expressions, arithmetic,
and function calls are not valid inside attribute argument lists.

#### Known Attributes

| Attribute | Valid on | Arguments | Purpose |
|---|---|---|---|
| `@extern("sym")` | `let`, `const` func/var | 1–2 strings | Bind to a C/OS/Vulkan symbol by name |
| `@extern("sym", "conv")` | `let`, `const` func/var | 2 strings | Same, with explicit calling convention |
| `@inline` | `let`, `const` func | none | Suggest the backend always inline this function |
| `@noinline` | `let`, `const` func | none | Prevent the backend from inlining this function |
| `@packed` | `struct` | none | Remove padding — all fields are byte-adjacent |
| `@deprecated("msg")` | func, var, struct | 0–1 string | Emit a warning at every use site |
| `@aot` | `main` entry point only | none | Use ahead-of-time compilation |
| `@jit` | `main` entry point only | none | Use just-in-time compilation via LLVM JIT |

`@inline` and `@noinline` are mutually exclusive on the same declaration.
`@aot` and `@jit` are mutually exclusive on the same declaration.

#### `@extern` — FFI Binding

`@extern("symbol")` declares that a function or variable is resolved by the
**linker** rather than compiled from Luc source. The body is omitted. The
calling convention defaults to `"C"` and may be overridden with a second
argument.

```
-- Grammar:
--   @extern(symbol_name)
--   @extern(symbol_name, calling_convention)
--
-- symbol_name        — the C/OS/Vulkan identifier the linker will look up
-- calling_convention — optional; default "C". Other values: "stdcall", etc.
```

**Rules:**

- `@extern` requires **`const`**, not `let`. An `@extern` binding is resolved
  permanently by the linker — using `let` would allow the binding to be
  replaced at runtime (e.g. `malloc = { return nil }`), which is meaningless
  for a linked symbol. The semantic pass emits **W3001** when `let` is used;
  compilation continues but you are warned to change it to `const`.

- `@extern` on a **function** — the declaration must have no body. The
  compiler emits an external function declaration in the LLVM IR; the linker
  resolves it.
  - **No body** (preferred) — no diagnostic.
  - **Empty body `= {}`** — **W3002** warning: the body is silently ignored;
    remove it to suppress the warning.
  - **Non-empty body** — **E3002** error: the body contains statements that
    will never execute; this is always a mistake.

- `@extern` on a **variable** — the declaration must have no initialiser. The
  linker provides the symbol's address.

- Raw pointer type `*T` is only valid in declarations carrying `@extern`.
  The semantic pass enables `*T` resolution when `@extern` is detected.

- `@extern` functions are fully callable from Luc code; the semantic pass
  validates argument count and types against the declared signature.

```luc
-- C stdlib bindings
@extern("malloc")
const malloc (size uint64) *uint8?

@extern("free")
const free (ptr *uint8)

@extern("printf", "C")
const printf (fmt *uint8, args ...any) int

-- Vulkan bindings
@extern("vkCreateInstance")
const vkCreateInstance (pInfo      *VkInstanceCreateInfo
                        pAllocator *VkAllocationCallbacks
                        pInstance  **VkInstance) uint32

-- Linker-provided constant (e.g. from a linker script)
@extern("__stack_top")
const stackTop *uint8
```

> **W3001 — using `let` instead of `const`:**
> ```luc
> @extern("malloc")
> let malloc (size uint64) *uint8?   -- W3001: should be 'const'
> ```
> Change `let` to `const`. The warning does not block compilation.

> **W3002 — empty body alongside `@extern`:**
> ```luc
> @extern("free")
> const free (ptr *uint8) = {}   -- W3002: empty body is ignored
> ```
> Remove `= {}`. The extern binding takes effect regardless.

> **E3002 — non-empty body alongside `@extern`:**
> ```luc
> @extern("malloc")
> const malloc (size uint64) *uint8? = {
>     return nil   -- E3002: this code will never run
> }
> ```
> Remove the body entirely.

#### `@packed` — Struct Layout

`@packed` removes all compiler-inserted padding from a struct. Every field is
placed at the next byte boundary after the previous field, regardless of
alignment requirements. This produces a layout identical to C's
`__attribute__((packed))` and is necessary for Vulkan push constants, file
format headers, and network packets.

```luc
@packed
struct VkPushConstant {
    modelMatrix  [16]float   -- 64 bytes, no padding inserted
    color        [4]float    -- 16 bytes, immediately follows
}

@packed
struct EthernetHeader {
    dest   [6]ubyte
    src    [6]ubyte
    etherType uint16
}
```

> **Note:** Using `@packed` on a struct with fields that require alignment
> greater than 1 byte can cause unaligned memory accesses, which are
> undefined behaviour on some architectures. Use only when the packed layout
> is required by an external protocol or API.

#### `@inline` / `@noinline` — Inlining Hints

`@inline` suggests to the LLVM backend that every call site of this function
should be inlined. `@noinline` prevents inlining even when the optimiser
would normally choose to inline.

```luc
@inline
const dot (a Vec3) (b Vec3) float = {
    return a.x*b.x + a.y*b.y + a.z*b.z
}

@noinline
const handleError (e Error) = {
    io.printl("error: " + e.message)
}
```

#### `@deprecated` — Deprecation Warning

`@deprecated` causes the compiler to emit a warning at every call site or use
of the annotated symbol. An optional message string explains the replacement.

```luc
@deprecated("use Vec3:normalise instead")
let normalize (v Vec3) Vec3 = { ... }

@deprecated
struct OldConfig { ... }
```

---

### Compiler Intrinsics

Compiler intrinsics look like function calls but are implemented directly by
the compiler backend — no function pointer, no call instruction, no overhead
beyond what the underlying hardware instruction requires.

**Syntax:** `@name(args)` in any expression position.

```
intrinsic_call  := '@' IDENTIFIER '(' [ intrinsic_arg_list ] ')'

intrinsic_arg_list := intrinsic_arg { ',' intrinsic_arg }

intrinsic_arg   := type                -- for @sizeof(T), @alignof(T), @bitcast(T, x)
                 | expr                -- all other intrinsics
```

The parser distinguishes type-argument intrinsics (`@sizeof`, `@alignof`,
`@bitcast`) from value-argument intrinsics at parse time. For all others,
arguments are parsed as regular expressions.

#### Categories

**Compile-time type queries** — resolved entirely at compile time. The result
is a compile-time constant (`isConst = true`) and may be used anywhere a
`const` initialiser is expected.

| Intrinsic | Arguments | Returns | Notes |
|---|---|---|---|
| `@sizeof(T)` | 1 type | `uint64` | Byte size of type `T` in memory |
| `@alignof(T)` | 1 type | `uint64` | Alignment requirement of `T` in bytes |

**Floating-point math** — maps to hardware-accelerated instructions via LLVM
overloaded intrinsics. The return type matches the argument type: if `x` is
`double`, the result is `double`.

| Intrinsic | Arguments | Returns | Notes |
|---|---|---|---|
| `@sqrt(x)` | 1 float/double | same as arg | Hardware square root |
| `@floor(x)` | 1 float/double | same as arg | Round toward −∞ |
| `@ceil(x)` | 1 float/double | same as arg | Round toward +∞ |
| `@round(x)` | 1 float/double | same as arg | Round to nearest, half away from zero |
| `@abs(x)` | 1 numeric | same as arg | Absolute value; works on integers and floats |
| `@pow(base, exp)` | 2 float/double | same as arg0 | Exponentiation |
| `@fma(a, b, c)` | 3 float/double | same as arg0 | Fused multiply-add: `(a * b) + c`, single rounding |
| `@min(a, b)` | 2 same-type | same as arg0 | Minimum value |
| `@max(a, b)` | 2 same-type | same as arg0 | Maximum value |

**Bit manipulation** — works on integer types only. Return type matches the
argument. Useful for low-level graphics and systems programming.

| Intrinsic | Arguments | Returns | Notes |
|---|---|---|---|
| `@clz(x)` | 1 integer | same as arg | Count leading zero bits |
| `@ctz(x)` | 1 integer | same as arg | Count trailing zero bits |
| `@popcount(x)` | 1 integer | same as arg | Count set (1) bits |
| `@bswap(x)` | 1 integer | same as arg | Reverse byte order (endianness swap) |

**Memory operations** — map to LLVM `memcpy`/`memmove`/`memset` intrinsics
with full hardware acceleration. All three are `void` (return no value).

| Intrinsic | Arguments | Returns | Notes |
|---|---|---|---|
| `@memcpy(dst, src, len)` | dest, src, uint64 | void | Copy `len` bytes; regions must not overlap |
| `@memmove(dst, src, len)` | dest, src, uint64 | void | Copy `len` bytes; handles overlap |
| `@memset(dst, val, len)` | dest, ubyte, uint64 | void | Fill `len` bytes with `val` |

**Unsafe / Vulkan** — raw bit-level operations with no runtime cost beyond
moving data. Use with care; incorrect use is undefined behaviour.

| Intrinsic | Arguments | Returns | Notes |
|---|---|---|---|
| `@bitcast(T, x)` | 1 type, 1 value | `T` | Reinterpret bits of `x` as type `T`; sizes must match |

#### Examples

```luc
-- Compile-time size queries
const vecSize  uint64 = @sizeof(Vec3)     -- 12 on typical platforms
const alignment uint64 = @alignof(float)  -- 4

-- Use in a const: @sizeof is isConst = true
const maxVerts uint64 = 65536
const bufBytes uint64 = maxVerts * @sizeof(Vertex)

-- Floating-point math
let len float = @sqrt(v.x*v.x + v.y*v.y + v.z*v.z)
let t   float = @clamp(@fma(a, b, c), 0.0, 1.0)   -- clamp via @min/@max
let t   float = @min(@max(@fma(a, b, c), 0.0), 1.0)

-- Bit manipulation
let bits uint32 = 0b1010_1100
let n    uint32 = @popcount(bits)    -- 4
let swap uint32 = @bswap(0xDEADBEEF) -- 0xEFBEADDE  (big/little endian swap)

-- Memory
let buf [*]ubyte = [*]ubyte {}
buf.reserve(1024)
@memset(buf[0], 0, @sizeof(Header))
@memcpy(dst.ptr, src.ptr, len)

-- Unsafe bit reinterpret (Vulkan / GPU use)
let asFloat float = @bitcast(float, bits)   -- interpret uint32 bits as float32
```

> **Note — `@sizeof` vs `float(x)` casts:** `@sizeof(T)` is a compiler
> intrinsic that returns the byte size of a type at compile time. It is
> entirely unrelated to explicit type casts like `float(x)` (which use the
> `from` system or built-in primitive conversion). Do not confuse the two.

> **Note — `@bitcast` vs unsafe `*T(x)` reinterpret:** The legacy pipeline
> unsafe reinterpret `*float(x)` (raw pointer cast in expression position)
> still works in `@extern` contexts for Vulkan struct reinterpretation.
> `@bitcast(T, x)` is the cleaner modern form for the same operation and
> does not require an `@extern` context.

## Choice and Fallback Operators

Luc provides unified operators for handling "Choice" types (unions like `T?` or `Expect<T>`).

### Standard Fallback (`??`)

The `??` operator extracts the "Success / Value" variant from a union. If the value is the "Error / Nil" variant, it evaluates and returns the right-hand side.

```
fallback_expr   := expr '??' expr
                   -- If LHS is T (success), result is T.
                   -- If LHS is Error or nil (failure), result is RHS.
```

### Pipeline Catch (`catch`)

`catch` is a built-in pipeline intrinsic used for error recovery. Unlike other pipeline steps, it **does not** short-circuit on `Error`.

```
catch_step      := expr '->' 'catch' '(' identifier ')' block
                   -- If upstream is T, the block is skipped and T is passed along.
                   -- If upstream is Error, the identifier is bound to the Error and the block executes.
```

### Forced Handling Rule

To ensure safety, any function returning `Expect<T>` **cannot be discarded**. If the result is not handled by `??`, `catch`, or variable assignment, the compiler generates a runtime check that **blocks / panics the thread** if an `Error` occurs.


## Operator Precedence (High → Low)

| Level | Operators | Associativity |
|---|---|---|
| 1 (highest) | `()` `.` `:` `.?` `[…]` `!` calls | left |
| 2 | unary `-` `not` `~` `&` | right |
| 3 | `^` (power) | right |
| 4 | `*` `/` `%` | left |
| 5 | `+` `-` | left |
| 6 | `<<` `>>` | left |
| 7 | `&&` (bitwise AND) `\|\|` (bitwise OR) `~^` (bitwise XOR) | left |
| 8 | `==` `!=` `<` `>` `<=` `>=` `===` | left |
| 9 | `and` | left |
| 10 | `or` | left |
| 11 | `??` | right |
| 12 | `->` (pipeline — runtime) | left |
| 13 | `+>` (composition — compile time) | left |
| 14 | `=` `+=` `-=` `*=` `/=` `^=` `%=` | right |
| 15 (lowest) | `if ?? else` (inline if expression) | right |

> **NOTE**
> On `if_expr` precedence:** `if cond ?? thenExpr else elseExpr` is a `primary_expr` — it begins with the `if` keyword so no infix precedence applies to its opening. However, the `else` clause is right-associative and binds with the lowest precedence of all, which means in a chain like `if a ?? b else if c ?? d else e` the second `if` is parsed as the `elseExpr` of the first — correct and expected. The `??` inside `if_expr` is not an infix operator; it is a fixed syntactic separator consumed by the `if_expr` production rule directly. Guard `??` (null-coalesce postfix op at level 11) is only parsed in that role when `??` appears **outside** an `if_expr` condition position.

## Literals

```
literal         := INT_LITERAL             -- 42
                 | FLOAT_LITERAL           -- 3.14
                 | STRING_LITERAL          -- "hello"
                 | RAW_STRING_LITERAL      -- r"raw\nno escaping"
                 | CHAR_LITERAL            -- 'a'
                 | HEX_LITERAL             -- 0xFF
                 | BINARY_LITERAL          -- 0b1010
```

### String Literals

A string literal is delimited by `"..."`. Escape sequences are processed by
the lexer — the resulting token value contains the decoded characters.

```
STRING_LITERAL  := '"' { char | escape_seq } '"'

escape_seq      := '\' escape_code

escape_code     := 'n'              -- newline        (LF, \u000A)
                 | 't'              -- tab             (\u0009)
                 | 'r'              -- carriage return (\u000D)
                 | '"'              -- literal double quote
                 | '\\'             -- literal backslash
                 | '0'              -- null character  (\u0000)
                 | 'x' HEX HEX     -- hex byte value  \xFF = byte 255
                 | 'u' HEX HEX HEX HEX
                                    -- Unicode codepoint (4 hex digits) \u0041 = 'A'
                 | 'U' HEX HEX HEX HEX HEX HEX HEX HEX
                                    -- Unicode codepoint (8 hex digits) \U0001F600 = 😀
```

```luc
let a string = "hello\nworld"          -- newline between words
let b string = "tab\there"             -- tab character
let c string = "quote: \""             -- literal "
let d string = "null\0term"            -- embedded null (C FFI)
let e string = "byte: \xFF"            -- hex byte 255
let f string = "unicode: \u0041"       -- 'A'
let g string = "emoji: \U0001F600"     -- 😀
```

### Raw String Literals

A raw string literal is prefixed with `r`. No escape sequences are processed —
every character is literal, including backslashes. Useful for regex patterns,
file paths, and any content where backslashes are common.

```
RAW_STRING_LITERAL := 'r"' { any char except '"' } '"'
```

```luc
-- without raw string: double-escaping required
let pattern string = "^\\d{3}-\\d{4}$"

-- with raw string: backslashes are literal
let pattern string = r"^\d{3}-\d{4}$"

-- Windows path
let path string = r"C:\Users\luc\documents\config.txt"
```

### Char Literals

A char literal holds a single character, delimited by `'...'`. The same
escape sequences as string literals are valid inside a char literal.

```
CHAR_LITERAL    := '\'' ( char | escape_seq ) '\''
```

```luc
let a char = 'A'
let b char = '\n'    -- newline character
let c char = '\''    -- literal single quote
let d char = '\\'    -- literal backslash
let e char = '\x41'  -- hex — same as 'A'
let f char = '\u0041' -- Unicode — same as 'A'
```

## Comments

Three comment forms are supported:

```luc
-- single-line comment (double dash, Lua style)
-- everything after -- on the same line is ignored

/- block comment
   spans multiple lines
   use for temporarily disabling code or long explanations
-/

/--
 - document comment — describes the next declaration
 - each continuation line starts with ' -'
 - '-' replaces '*' from JavaDoc / C-style doc comments
 -
 - attached to the immediately following top-level declaration
 - (func, struct, type, impl)
--/
```

### Comment grammar

```
line_comment    := '--' { any char except newline }

block_comment   := '/-' { any char } '-/'
                   -- may span multiple lines
                   -- nesting is NOT supported

doc_comment     := '/--' { ' -' line_content } '--/'
                   -- must immediately precede a top-level declaration
                   -- each body line begins with ' -'
```

### Lexer note

Block comment `/-` and doc comment `/--` both start with `/`. The lexer
disambiguates by peeking one character ahead:

- `/` followed by `-` followed by `-` → doc comment `/--`
- `/` followed by `-` → block comment `/-`
- `/` alone → `DIV` token

## Documentation Comments

Luc has three forms of documentation comment. All three are stored in the AST
and shown in LSP hover. Only visibility controls what appears in generated
output — not the comment form itself.

### Three doc forms

**Form 1 — stacked `--` line comments above a declaration**

Consecutive `--` lines ending on the line immediately before the declaration
are treated as its documentation. This is the lightest syntax — no special
delimiter, just regular comments stacked above:

```luc
-- normalizes the vector in place
-- only call this after the vector has been validated
let normalize (v Vec2) Vec2 = { ... }
```

**Form 2 — trailing `--` comment on the same line**

A `--` comment on the same line as a declaration is its inline documentation.
Best for short descriptions — struct fields, simple variables:

```luc
struct Vertex {
    pos   Vec3   -- world-space position
    color Vec4   -- RGBA, linear color space
    uv    Vec2   -- texture coordinates, 0..1
}

let maxVertices int = 65536   -- Vulkan hard limit on this device
```

**Form 3 — `/-- --/` block comment**

The block form for longer, multi-paragraph documentation. Each body line
begins with ` -`. Markdown content is supported:

```luc
/--
 - Computes the dot product of two vectors.
 -
 - Returns a scalar value equal to `|a| * |b| * cos(angle)`.
 - Result is zero if vectors are perpendicular.
--/
pub impl Vec2 {
    dot (other Vec2) float = { ... }
}
```

### Attachment rules

The parser determines which declaration a doc comment belongs to using these
rules, applied in order:

```
doc_attachment  := stacked_line_doc
                 | trailing_line_doc
                 | block_doc

stacked_line_doc := line_comment+
                    -- consecutive '--' lines, last line is (decl_line - 1)
                    -- zero blank lines between comment and declaration

trailing_line_doc := decl line_comment
                     -- '--' comment on the same line as the declaration

block_doc       := doc_comment
                   -- '/-- --/' closing '--/' is on (decl_line - 1)
                   -- zero blank lines between block and declaration
```

**Rule 1 — stacked line comments above**
```luc
-- line one
-- line two
let x int = 5       -- attaches: "line one\nline two"
```

**Rule 2 — blank line breaks attachment**
```luc
-- this is floating

let x int = 5       -- x has NO doc comment
```

**Rule 3 — trailing comment**
```luc
let x int = 5       -- the variable has value 5
```

**Rule 4 — stacked above + trailing — above wins**
```luc
-- above comment
let x int = 5       -- trailing comment
-- result: "above comment" is the doc; trailing is ignored
```

**Rule 5 — `/-- --/` block above**
```luc
/--
 - block doc
--/
let x int = 5       -- attaches the block
```

**Rule 6 — mixed: `--` lines above a `/-- --/` block — only block attaches**
```luc
-- this is floating (precedes the block, not the declaration)
/--
 - this attaches
--/
let x int = 5
```

**Rule 7 — doc comment inside impl block attaches to the following method**
```luc
pub impl Vec2 {
    -- computes dot product of two vectors
    dot (other Vec2) float = { ... }    -- attaches to dot()
}
```

### Visibility and doc output

All doc comments are stored in the AST regardless of visibility. The visibility
of the declaration controls where the doc appears:

| Declaration | LSP hover | Generated docs |
|---|---|---|
| `pub` + in module manifest | ✅ anyone | ✅ external consumers |
| `pub` + no module manifest | ✅ anyone | ✅ all importers |
| `pub` + not re-exported | ✅ package | ❌ external consumers |
| private (no `pub`) | ✅ package | ❌ not emitted |

The rule: **always store, always show in LSP, filter by visibility only at
doc generation time.**

### Content format

Doc comment content is **Markdown**. The ` -` line prefix is stripped before
parsing — the remaining text is treated as Markdown prose. This applies to
all three forms: stacked `--`, trailing `--`, and `/-- --/` block.

```luc
/--
 - Computes `a + b`.
 -
 - ## Example
 - ```luc
 - let result int = add(1)(2)   -- 3
 - ```
--/
let add (a int) (b int) int = { return a + b }
```

## Keywords (Reserved)

```
pub export package module use as impl trait type from
let const struct enum
async await parallel
bool byte short int long ubyte ushort uint ulong
int8 int16 int32 int64 uint8 uint16 uint32 uint64
float double decimal string char any nil
if else match switch case default is
while for in do return break continue
and or not true false
```