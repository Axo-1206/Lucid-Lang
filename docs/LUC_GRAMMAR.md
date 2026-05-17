# Luc — Grammar Reference

> [!TIP] Scope of this file:
> Formal grammar rules for the Luc parser. Code examples are in `LUC_EXAMPLES.md`. Project identity is in `LUC_PROJECT_OVERVIEW.md`.

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

---

## Top-Level Structure

```
program         := package_decl { top_level_decl }

package_decl    := 'package' IDENTIFIER

top_level_decl  := { attribute } actual_decl
                   -- Zero or more '@' attributes may precede any declaration.
                   -- Attributes are consumed and attached to the declaration that follows.

actual_decl     := use_decl
                 | enum_decl
                 | struct_decl
                 | trait_decl
                 | type_decl
                 | impl_decl
                 | from_decl
                 | var_decl
                 | func_decl
```

### Entry Point (main)

```luc
export const main () -> int = {
    return 0
}

-- with command-line arguments
export const main (args []string) -> int = {
    return 0
}
```

### Compilation Mode Directives

`@aot` and `@jit` are mutually exclusive attributes valid only on `main`:

```
@aot   -- ahead-of-time: produces a native binary at build time
@jit   -- just-in-time:  compiles and executes at runtime via LLVM JIT
```

```luc
@aot
export const main () -> int = {
    return 0
}
```

---

## Module System

Every `.luc` file is a module. The module's identity is its file path relative
to the package root. Modules are flat; nesting is not supported.

### Visibility — three tiers

| Keyword  | Scope       | Access                                                     |
| -------- | ----------- | ---------------------------------------------------------- |
| (none)   | **File**    | Only visible within the same `.luc` file                   |
| `pub`    | **Package** | Visible to all files sharing the same `package` identifier |
| `export` | **World**   | Visible to external consumers of the package               |

- `pub` and `export` are top-level modifiers only — **illegal** inside blocks or
  function bodies. Declarations inside blocks are implicitly private to that block.
- `pub impl` / `export impl` controls method accessibility at the block level when used at top-level.
- `pub from` / `export from` controls converter accessibility at the block level when used at top-level.
- If a package has zero `export` declarations it is a private package.
- Most declarations (struct, enum, type, trait, impl, from, let, const, func) are allowed in local scopes.

### Re-Exports

```luc
-- math/api.luc
package math

export use math.vec2           -- re-export all pub items from vec2
export use math.matrix.Mat2    -- granular re-export of a single type
```

### Import

```
use_decl        := [ visibility_mod ] 'use' module_path [ 'as' IDENTIFIER ]

module_path     := IDENTIFIER { '.' IDENTIFIER }
```

```luc
use math              -- all exported items from the math package
use math.vec2         -- specific file
use math as m         -- local alias: m.Vec2
```

> [!NOTE]
> **Local usage:** `use` can be declared inside any block. When local, `visibility_mod` must be omitted.

---

## Types

> [!WARNING] No union type, use the 'any' type instead
> Luc does **not** have union types (`T | U`). Use the `any` type together with `is` checks or pattern matching to handle multiple types at runtime.

```
type            := base_type [ generic_args ] [ '?' ]
                 | ref_type
                 | ptr_type
                 | array_type
                 | func_type

base_type       := primitive_type
                 | IDENTIFIER

primitive_type  := 'bool'
                 | 'byte'  | 'short'  | 'int'   | 'long'
                 | 'ubyte' | 'ushort' | 'uint'  | 'ulong'
                 | 'int8'  | 'int16'  | 'int32' | 'int64'
                 | 'uint8' | 'uint16' | 'uint32'| 'uint64'
                 | 'float' | 'double' | 'decimal'
                 | 'string'| 'char'
                 | 'any'

-- Nullable suffix: attaches to value types only.
-- Generic arguments always come before '?'.
-- '?' is NEVER valid directly on an inline function type — use a named alias.
nullable_suffix := '?'

-- Reference (&T) — safe managed reference
ref_type        := '&' type

-- Raw pointer (*T) — allowed anywhere, but operations (dereference, indexing,
-- arithmetic) are forbidden. Use only for storage, nil checks, and intrinsics.
ptr_type        := '*' type

-- Array types
array_type      := '[' INT_LITERAL ']' type   -- fixed:   [100]int
                 | '[' ']' type               -- slice:   []int
                 | '[' '*' ']' type           -- dynamic: [*]int
                 | array_type array_type       -- nested:  [][*]float

-- Generic arguments — always before '?'
generic_args    := '<' type { [','] type } '>'

-- Function type: qualifiers live here in type aliases and parameter types.
-- Anonymous function VALUES do not carry qualifiers (see Anonymous Functions).
func_type       := [ qualifier_list ] param_group { param_group } [ '->' return_list ]

qualifier_list  := { '~' IDENTIFIER }

return_list     := '(' [ return_type { ',' return_type } ] ')'   -- multiple returns
                 | return_type                                    -- single return

return_type     := type
                 | param_group { param_group } '->' return_list
```

### Nullable Rules

Two distinct mechanisms — they never overlap:

```
~nullable qualifier  -- function bindings only (see Qualifiers section)
? postfix            -- value types only: primitives, structs, arrays, named aliases
```

```luc
-- nullable value types
let a  int?        = nil
let a  Vec2?       = nil
let xs List<int>?  = nil     -- generic before ?, always
let xs List<int?>  = nil     -- list of nullable int — different meaning

-- nullable function binding
let f  ~nullable (a int) -> int = nil
let f  ~async ~nullable (a int) -> int = nil

-- function returning nullable value
let f  (a int) -> int?  = { ... }
let f  (a int) -> Vec2? = { ... }

-- function returning nullable function — alias required
type Transform = (v Vec2) -> Vec2
let f  (a int) -> Transform? = { ... }

-- ? is NEVER valid directly after an inline function type as a whole (i.e., nullable function type). Use a type alias for that: type NullableFunc = (int) -> int?. However, (int) -> int? (nullable return type) is allowed. 
-- let f (a int) -> ((b int) -> int)?   -- ERROR: use an alias
```

---

## Value and Reference Semantics

Every type is either **owned** or **borrowed**. Bare `T` = owned, `&T` = borrowed.

### Owned types — copied on assignment

| Type           | Syntax                           | Storage                         | On assignment                      |
| -------------- | -------------------------------- | ------------------------------- | ---------------------------------- |
| Primitives     | `bool` `int` `float` `char` …    | stack / inline                  | full copy                          |
| Enum           | `Direction.North`                | integer (`byte`/`short`)        | full copy                          |
| Fixed array    | `[N]T`                           | stack / inline                  | full element copy                  |
| Slice          | `[]T`                            | fat pointer (`ptr + len + cap`) | copies view header — shares buffer |
| Dynamic array  | `[*]T`                           | heap-owned buffer               | full deep copy                     |
| String         | `string`                         | heap-owned sequence             | full deep copy                     |
| Struct         | `Vec2` `Player` …                | inline / stack                  | full deep copy                     |
| Named function | `add` `Vec2:normalize`           | function pointer                | pointer copy                       |
| Closure        | `add(10)` `(x int) -> int { … }` | heap-allocated env              | copies reference to env            |

### Struct deep copy

Struct assignment always produces a fully independent value. Owned fields are
cloned, borrowed (`&T`) fields copy their reference only.

```luc
struct Player {
    score  int        -- owned: cloned
    items  [*]string  -- owned: buffer deep-copied
    world  &World     -- borrowed: reference copied, World not cloned
}

let a Player = Player { score = 10  items = ["sword"]  world = &w }
let b Player = a
-- b.score and b.items are independent; b.world shares the same World as a.world
```

### Borrowed types — share without copying

```luc
let a Player = Player { … }

let ref  &Player = a    -- mutable shared reference
const rc &Player = a    -- read-only shared reference
```

| Declaration             | Copies?     | Field mutation?       | Owner |
| ----------------------- | ----------- | --------------------- | ----- |
| `let b Player = a`      | ✅ deep copy | ✅ b's own fields      | `b`   |
| `let ref &Player = a`   | ❌ shared    | ✅ visible through `a` | `a`   |
| `const ref &Player = a` | ❌ shared    | ❌ read-only           | `a`   |

### Circular references

```luc
struct Node {
    value int
    next  &Node?    -- borrowed reference, nullable
}
```

### Function values and closures

Named functions and impl methods are plain function pointers — no captured state.
Closures (partial applications, anonymous functions capturing variables) hold a
heap-allocated environment. Assigning a closure copies the reference to that
environment.

### `any` and boxing

A value stored in `any` is boxed with a runtime type tag. Owned types placed
into `any` are deep-copied into the box. Borrowed references (`&T`) store the
reference — the box does not own the referenced value.

---

## The Sealed Conduit Model (Raw Pointers)

Raw pointers (`*T`) are sealed conduits — carry them, pass them to extern
functions, check for nil, but never dereference directly.

### Allowed operations

1. Store in a variable, struct field, or parameter
2. Pass to an `@extern` function
3. Nil check (`== nil`, `!= nil`)
4. Pass to pointer intrinsics (`#ptrToRef`, `#ptrOffset`, etc.)
5. Print the address for debugging

### Forbidden operations (compiler error)

- Dereferencing: `*ptr`
- Field access: `ptr.field`
- Indexing: `ptr[i]`
- Arithmetic: `ptr + 4` — use `#ptrOffset` instead
- Assignment: `*ptr = value`

### Boundary crossing (intrinsics)

```luc
#ptrToRef(ptr)   -- *T → &T  (assert validity, cross to safe reference)
#refToPtr(ref)      -- &T → *T  (convert back to raw pointer)
#ptrOffset(ptr, n)  -- pointer arithmetic, returns new *T
#ptrDiff(p1, p2)    -- distance between two pointers as int64
```

```luc
@extern("malloc")
const malloc (size uint64) -> *uint8?

let buf *uint8? = malloc(1024)
if buf == nil { return 1 }

let ref &uint8 = #ptrToRef(buf)    -- cross the boundary
ref = 0xFF                                  -- work with it safely

let next *uint8? = #ptrOffset(buf, 1)      -- pointer arithmetic
```

---

## Variable Declaration

```
var_decl        := [ visibility_mod ] decl_keyword IDENTIFIER type_ann [ '=' expr ]

decl_keyword    := 'let' | 'const'

type_ann        := type
```

> [!WARNING] No type inference.  
> Luc does not infer types. Every declaration **must** include an explicit type annotation. The compiler rejects any declaration without one. 
> ```luc
> let x     = 5     -- ERROR: type annotation required
> let x int = 5     -- OK
> ```

### Declaration Semantics

| Keyword | Reassignable | Mutable in place | Value known at | Nil allowed             |
| ------- | ------------ | ---------------- | -------------- | ----------------------- |
| `let`   | ✅            | ✅                | runtime        | ✅ (if type is nullable) |
| `const` | ❌            | ❌                | compile time   | ❌                       |

`const` requires a compile-time constant initialiser: a literal, another `const`
name, an enum variant, or arithmetic over those. Function calls, struct literals,
and runtime values are rejected by the semantic pass.

```luc
const MAX  int   = 65536
const PI   float = 3.14159
const HALF float = PI / 2.0
const STAGE int  = ShaderStage.Vertex

const x int = readInput()        -- ERROR: function call is not compile-time
const v Vec2 = Vec2 { x = 0.0 } -- ERROR: struct literal is not compile-time
```

### Non-Nullable Variables and Memory

A non-nullable variable (e.g. `int`, `Vec2`, `string`) **can never be set to
nil**. This is intentional — it eliminates null pointer exceptions for these
types. The compiler guarantees that if a non-nullable variable exists it holds
a valid value.

- **Primitive types** (`int`, `float`, `bool`, `char`) live on the stack. When
  the scope ends they are gone automatically — no manual release needed.
- **Heap-allocated types** (`string`, `[*]T`) are cleaned up when the variable
  goes out of scope. To free the buffer **early** while the scope is still live,
  call `.clear()` — the variable remains valid as an empty collection, never nil.

```luc
let bigData [*]byte = loadFile("huge.bin")
-- ... use bigData ...
bigData.clear()    -- free heap buffer early; bigData is now empty but valid
```

Use nullable types (`int?`, `Vec2?`) when a value genuinely may be absent.
Non-nullable types express the guarantee that a value is always present.

### Multiple Assignment

A function returning multiple values can be assigned to multiple variables in a
single statement. Each variable requires its own explicit type annotation.

```
multi_assign    := decl_keyword var_spec { ',' var_spec } '=' expr   -- declaration
var_spec        := IDENTIFIER type_ann
decl_keyword    := 'let' | 'const'

-- Reassignment to existing variables (no let/const)
multi_assign_stmt := expr_lhs { ',' expr_lhs } '=' expr

expr_lhs        := IDENTIFIER
                 | expr '.' IDENTIFIER
                 | expr '[' expr ']'
                 -- any expression that can be an lvalue (assignable)
```

> [!NOTE] 
> **For Reassignment to existing variables (no let/const)**
> - Each expr_lhs must be a valid lvalue (assignable location): a variable name, a field access, or an array/slice index. Function calls, literals, and other value expressions are not allowed.
> - The right‑hand side expr must evaluate to exactly as many return values as there are left‑hand side expressions.
> - The values are assigned from left to right.
> - This statement is only allowed in block scopes – not at top level.
> - Assigning to a const variable is a semantic error.

```luc
-- two return values
let value int, msg string = doSomething()

-- shorthand without per-variable keyword is NOT supported
let value, msg int = ...          -- ERROR: each variable needs its own type
let value int, msg string = ...   -- OK: implicit let on second variable

/--
 - multiple reassignment, A function that returns multiple values can be assigned 
 - to multiple variables in a single statement, either at declaration time or later 
 - as a reassignment.
--/
let a int, b string = f()     -- declaration
a, b = g()                     -- reassignment (multi_assign_stmt)
x, y = z, w                    -- ERROR: RHS must be single expression
```

> [!WARNING]
> No type inference in multi-assignment, every variable must have an explicit type annotation.

### Declaration form
Every variable has its own explicit type annotation, and the keyword (`let` or `const`) applies to all:

```luc
let q int, r int = divmod(10, 3)
const w int, h int = getScreenSize()
```

### Reassignment form (to already declared variables)
After variables are declared, you can reassign them using a multi‑assignment statement:

```luc
let q int, r int -- first declare
q, r = divmod(20, 6) -- reassign

let x float, y float
x, y = 3.14, 2.718 -- ERROR: RHS must be a single expression
```
> [!NOTE]
> The right‑hand side must be a single expression that returns as many values as there are left‑hand side targets. Comma‑separated literal expressions are not allowed.

---

## Qualifiers

Qualifiers are **call-site metadata on the binding**. They are not part of the
type identity — two functions with identical parameter and return types are the
**same type** regardless of qualifiers.

```
qualifier       := '~' IDENTIFIER

known qualifiers:
    ~async      -- call site must use await
                -- body may use await on calls to other ~async functions
    ~nullable   -- call site should guard for nil before calling
    ~parallel   -- call site executes the function in parallel
                -- body has parallel restrictions (see below)
```

### Qualifier Rules

**Not part of type identity** — qualifiers are ignored for type equality:

```luc
-- all three are the same type: (a int) -> int
let f ~async    (a int) -> int = { ... }
let g ~nullable (a int) -> int = nil
let h           (a int) -> int = { ... }

type Op = (a int) -> int

let op Op = f    -- valid
let op Op = g    -- valid
let op Op = h    -- valid
```

**Qualifiers live on the binding, not the value.** You can assign any
same-signature function to a qualified binding:

```luc
let f ~async (a int) -> int = { ... }

let g (a int) -> int = { return a * 2 }
f = g    -- valid: same signature. f still carries ~async after reassignment.
```

**Qualifier order is ignored for type equality.** Canonical display order is
alphabetical:

```luc
-- identical
let f ~async ~nullable (a int) -> int = nil
let f ~nullable ~async (a int) -> int = nil
```

**Generics belong to the identifier, before qualifiers:**

```luc
let parallelFor<T> ~parallel (items [*]T, body (item T)) = { ... }
let fetch<T>       ~async    (url string) -> T = { ... }
```

**Valid contexts for qualifiers:**

```
function declaration       let f ~async (a int) -> int = { ... }
parameter type             (task ~async () -> int)
type alias                 type AsyncOp = ~async (a int) -> int
inline return after ->     (a int) -> ~nullable (b int) -> int
```

> [!WARNING]
>  - **Qualifiers are NOT valid on anonymous function values.** An anonymous
> function is a plain value. Its qualifier context is determined by the
> declaration or parameter type it is assigned to, not by the value itself.
> See the Anonymous Functions section for details.
> - **Qualifiers on parameters are hints to the caller, not type gates.** The
qualifier documents intent — it does not prevent a non-qualified function from
being passed:

```luc
-- ~async on the parameter hints that task will be awaited inside run
let run (task ~async () -> int) -> int = {
    return await task()
}

-- both are valid — same underlying signature
run((url string) -> int { return 42 })             -- no qualifier on value
run((url string) -> int { return await fetch() })  -- ~async would be on declaration
```

### Call-Site Behavior

**`~async` — requires `await`:**

When the compiler sees a call to a `~async`-qualified binding, it requires
`await` at the call site. The `await` keyword tells the compiler to check the
callee's declaration for the `~async` qualifier:

```luc
let fetch ~async (url string) -> string = { ... }

fetch("url")           -- ERROR: ~async binding must be called with await
await fetch("url")     -- OK

-- await is only valid inside a ~async function body
let process ~async (url string) -> string = {
    let raw string = await fetch(url)    -- OK: fetch is ~async, process is ~async
    return raw
}

let bad (url string) -> string = {
    return await fetch(url)    -- ERROR: await inside non-async function body
}
```

> [!NOTE]
> When the compiler encounters `await expr`, it:
> 1. Evaluates `expr` — must resolve to a function call
> 2. Looks up the function being called
> 3. Checks whether that function's declaration carries the `~async` qualifier
> 4. If yes: valid, suspend and wait for result
> 5. If no: ERROR — cannot await a non-async function
>
> `await` is only valid inside a function body whose declaration carries `~async`.

**`~nullable` — requires nil guard:**

```luc
let handler ~nullable (e Event) = nil

handler(event)           -- WARNING: ~nullable called without nil guard
if handler != nil {
    handler(event)       -- OK: guarded
}
```

**`~parallel` — body restrictions:**

When a function is called through a `~parallel`-qualified binding, the compiler
enforces these restrictions inside the body function argument:

- No `return` statements
- No `break` or `continue`
- No `await` expressions
- No writes to variables declared outside the body scope

```luc
let parallelFor<T> ~parallel (items [*]T, body (item T)) = { ... }

parallelFor(mesh.vertices, (vertex Vertex) {
    vertex.pos = vertex.pos |> transform    -- OK: local to this iteration
    result = 5                              -- ERROR: writes to outer scope
    return                                  -- ERROR: return in parallel body
    await fetch()                           -- ERROR: await in parallel body
})
```

### `~parallel` Replaces the `parallel` Keyword

The `parallel` keyword is removed. Parallel execution is expressed through
`~parallel` library functions. This gives developers full control over the
execution strategy:

```luc
-- data-parallel iteration
let parallelFor<T> ~parallel (items [*]T, body (item T)) = { ... }

parallelFor(mesh.vertices, (vertex Vertex) {
    vertex.pos    = vertex.pos    |> transform
    vertex.normal = vertex.normal |> normalize
})

-- task-parallel execution
let parallelRun ~parallel (tasks [*]()) = { ... }

parallelRun([
    () { loadTextures()   },
    () { compileShaders() },
    () { loadMeshes()     }
])

-- custom strategies
let parallelGPU<T>     ~parallel (items [*]T, body (v T))             = { ... }
let parallelSIMD<T>    ~parallel (items [*]T, body (f T))             = { ... }
let parallelChunked<T> ~parallel (items [*]T, size int, body (c []T)) = { ... }
```

### Composition `+>` and Qualifiers

`+>` returns a plain function with **NO qualifiers**. Qualifiers on the result come
from the binding, not from the components:

```luc
-- result is plain regardless of component qualifiers
let pipeline = fetchData +> processData

-- assign qualifier to the result binding if needed
let pipeline ~async = fetchData +> processData

-- nullable components must be guarded before composing
if transform != nil {
    let h = transform +> render
}
```

### Type Aliases with Qualifiers

Since qualifiers are valid on function types, type aliases can carry them:

```luc
type AsyncOp      = ~async    (a int) -> int
type NullableOp   = ~nullable (a int) -> int
type ParallelBody = ~parallel (item int)
type AsyncMaybeOp = ~async ~nullable (a int) -> int
```

---

## Function Signatures

A function signature has three parts: qualifiers, parameter groups, and the
return boundary `->`.

```
func_decl       := [ visibility_mod ] decl_keyword IDENTIFIER [ generic_params ]
                   [ qualifier_list ] param_group { param_group }
                   [ '->' return_list ]
                   '=' func_body

> [!NOTE]
> **Local usage:** Functions can be declared inside any block. When local, `visibility_mod` must be omitted.

qualifier_list  := { '~' IDENTIFIER }

param_group     := '(' [ param_list ] ')'

param_list      := param { [','] param } [ [','] variadic_param ]

param           := IDENTIFIER type         -- name then type (Go style)

variadic_param  := IDENTIFIER '...' type   -- e.g. args ...int

return_list     := return_type { ',' return_type }

return_type     := type
                 | param_group { param_group } '->' return_list
                   -- inline curry function as return type
                   -- prefer type alias for anything non-trivial

func_body       := block
                 | anon_func
```

### The Return Boundary `->`

`->` separates the parameter groups from the return types. Everything before
`->` is input, everything after `->` is output.

**Void function** — omit `->` entirely:

```luc
let log   (msg string)
let clear ()
```

**Single return:**

```luc
let add  (a int, b int) -> int
let name (id int)       -> string
```

**Multiple return** — comma separated after `->`:

```luc
let f (a int) -> (int, string)
let g (src string) -> (int, bool, string)
```

> [!TIP]
> **Good practice — write each return type on its own line if one of the return types is a function:**
>
> ```luc
> -- Bad: return type too complex
> let parse (src string) -> (int, string, (a float) -> int)
> 
> -- Good: each return type on its own line
> let parse (src string) -> (
>           int,
>           string,
>           (a float) -> int
> ) = {
>     ...
> }
> ```

### Currying

Multiple parameter groups express curry. Each `()` group is one call step.
The compiler desugars this into a function that returns another function.

```luc
-- give add an int, get back (b int) -> int
let add (a int)(b int) -> int = { return a + b }

add(10)       -- returns (b int) -> int
add(10)(5)    -- returns 15

-- deep curry
let clamp (min int)(max int)(value int) -> int = {
    if value < min { return min }
    if value > max { return max }
    return value
}

let clamp0to100 = clamp(0)(100)    -- (value int) -> int
clamp0to100(42)                     -- 42
clamp0to100(150)                    -- 100

-- chains naturally with |> and +>
let addTen = add(10)
42 |> addTen |> string              -- "52"

let process = add(10) +> string     -- (b int) -> string
process(5)                           -- "15"
```

The compiler desugars multiple parameter groups into a function that explicitly
returns an anonymous function. The captured arguments become closure state.
You always write the sugar form:

```luc
-- what you write
let add (a int)(b int) -> int = { return a + b }

-- what the compiler produces internally (never write this yourself)
let add (a int) -> (b int) -> int = { return (b int) -> int { return a + b } }
```

To close over a local variable (not a parameter), write the anonymous function
explicitly:

```luc
let multiplier int = 3
let scale = (x int) -> int { return x * multiplier }
```

### Returned Curry Functions

A return type can itself be a curry function type. The `,` separates top-level
return items. Each `->` belongs to its nearest preceding parameter group chain.

```luc
let f (a int) -> (
    int,
    (s string)(n float) -> int
) = {
    ...
}
```

**Reading this:**

- `f` takes an `int`
- The first `->` is the return boundary for `f`
- `f` returns two things: an `int`, and a curry function `(s string)(n float) -> int`
- The second `->` belongs to the inner function `(s string)(n float)`, not to `f`

**Mental model — each `->` is a scope wrapper:**

```
f(int) -> (
    int
    |
    (s string)(n float) -> (
        int
    )
)
```

> [!TIP]
> **Good practice — use type aliases for complex return types:**
>
> ```luc
> type FloatParser = (s string)(n float) -> int
>
> -- we don't need to write the function type in a new line if we use type alias
> let f (a int) -> (int, FloatParser) = {
>     ...
> }
> ```

### Anonymous Functions

An anonymous function is a **plain value** — it has no qualifiers. The
qualifier context comes from the declaration or parameter type the anonymous
function is assigned to, not from the function value itself.

```
anon_func   := param_group { param_group } [ '->' return_list ] block
               -- NO qualifier_list
               -- qualifiers belong to declarations, parameter types, and aliases
```

```luc
-- simple anonymous function
let double = (x int) -> int { return x * 2 }

-- anonymous function stored in an ~async binding
-- the qualifier is on the binding, not on the value
let fetch ~async (url string) -> string = (url string) -> string {
    return await httpGet(url)
}

-- shorthand block body (preferred)
let fetch ~async (url string) -> string = {
    return await httpGet(url)
}

-- anonymous function as argument — type is checked against parameter type
-- the parameter type carries the qualifier, not the passed value
let run (task ~async () -> int) -> int = {
    return await task()
}

run(() -> int { return 42 })           -- valid: signature matches
run(() -> int { return await fetch() }) -- also valid: body uses await correctly
                                        -- because fetch is ~async
```

> [!IMPORTANT]
> **Why no qualifiers on anonymous functions?**
> A qualifier describes how the compiler treats a **call site**. When a function is called, the compiler looks up the **binding's declaration** (or the **parameter type**) to find qualifiers — it never inspects the value itself. Therefore a qualifier on an anonymous function value is unreachable and meaningless. Qualifiers on values are removed from the language to avoid confusion.

### Generic Functions

Generic functions allow code to operate on multiple types using type parameters.

```
generic_params  := '<' generic_param { [','] generic_param } '>'

generic_param   := IDENTIFIER
                 | IDENTIFIER ':' IDENTIFIER
                 | IDENTIFIER ':' constraint_list

constraint_list := IDENTIFIER { '+' IDENTIFIER }
```

#### Syntax

Generic parameters come immediately after the function name, before any parameter groups or qualifiers.

```luc
let identity<T> (v T) -> T = { return v }

let map<T, U> (items [*]T, f (item T) -> U) -> [*]U = { ... }
```

#### Type Constraints

Type parameters can be constrained to traits using the `:` syntax. Multiple traits are joined with `+`.

```luc
let printSorted<T : Comparable + Printable> (items []T) = { ... }
```

#### Instantiation

Generic functions can be called with explicit type arguments.

```luc
-- Explicit instantiation
let x int = identity<int>(42)   -- [1]
```

> [!NOTE]
> No type inference for generic functions — type must always be specified.

#### Generic Qualifiers

Qualifiers follow the generic parameter list:

```luc
let fetch<T> ~async (url string) -> T = { ... }
```

### Complete Signature Reference

```luc
-- void
let log (msg string)

-- single return
let add (a int, b int) -> int

-- multiple return
let f (a int) -> (int, string)

-- multiple return, formatted
let parse (src string)
    -> (
        int,
        (a float) -> int
    )
= { ... }

-- curry
let add (a int)(b int) -> int

-- deep curry
let clamp (min int)(max int)(value int) -> int

-- curry with multiple return
let f (a int)(b string) -> (int, string)

-- qualifiers
let fetch ~async    (url string) -> string
let find  ~nullable (items [*]int, pred (item int) -> bool) -> int

-- generic (generic before qualifiers, always)
let map<T, U> (items [*]T, f (item T) -> U) -> [*]U

-- returned curry function, formatted
let f (a int) -> (
    int,
    (s string)(n float) -> int
) = { ... }

-- nullable return value
let f (a int) -> int?

-- nullable function binding
let f ~nullable (a int) -> int = nil

-- nullable returned function via alias
type Transform  = (v Vec2) -> Vec2
let f (a int) -> Transform? = { ... }

-- curry returning nullable function inline
let f (a int) -> ~nullable (b int) -> int
```

---

## Struct Declaration

```
struct_decl     := [ visibility_mod ] 'struct' IDENTIFIER [ generic_params ]
                   '{' { field_decl } '}'

> [!NOTE]
> **Local usage:** Structs can be declared inside any block. When local, `visibility_mod` must be omitted.

field_decl      := IDENTIFIER type [ '=' expr ]    -- name then type, optional default

generic_params  := '<' generic_param { [','] generic_param } '>'

generic_param   := IDENTIFIER
                 | IDENTIFIER ':' IDENTIFIER
                 | IDENTIFIER ':' constraint_list

constraint_list := IDENTIFIER { '+' IDENTIFIER }
```

### Struct Literals

```
struct_literal  := IDENTIFIER '{' { field_init } '}'
                 | IDENTIFIER generic_args '{' { field_init } '}'

field_init      := IDENTIFIER '=' expr
                   -- always '=', never ':'
```

### Field Assignment

```
field_assign    := expr '.' IDENTIFIER '=' expr
                   -- only valid if containing variable is 'let'
```

### Examples

```luc
pub struct Vec2 {
    x float
    y float
}

struct Color {
    r float = 1.0    -- default value
    g float = 1.0
    b float = 1.0
    a float = 1.0
}

struct Scene<T : Drawable> {
    objects []T
}

struct Cache<K : Hashable + Comparable, V> {
    keys   []K
    values []V
}

-- struct literal
let origin Vec2  = Vec2  { x = 0.0  y = 0.0 }
let white  Color = Color {}                     -- all fields take defaults
let p Vec2<int>  = Vec2<int> { x = 1  y = 2 }

-- field access
let x float = origin.x       -- read
origin.x = 5.0               -- write (only valid if origin is 'let')
```

---

## Member Access

```
member_access   := expr '.' IDENTIFIER        -- data member: field from struct body
                 | IDENTIFIER ':' IDENTIFIER  -- behavior member: method from impl block
```

- `.` — data field access. Reassignable if the containing variable is `let`.
- `:` — impl method access. Never reassignable. Behavior members are plain
  function references and can be passed as values, stored in typed variables,
  or used as pipeline steps.

```luc
v.x              -- read field
v.x = 5.0        -- write field (let only)
Vec2:normalize   -- function reference to normalize from Vec2's impl
Vec2:length      -- function reference to length from Vec2's impl
Vec2:length = .. -- ERROR: impl methods cannot be reassigned
```

---

## Enum Declaration

```
enum_decl       := [ visibility_mod ] 'enum' IDENTIFIER
                   '{' enum_variant { [','] enum_variant } '}'

> [!NOTE]
> **Local usage:** Enums can be declared inside any block. When local, `visibility_mod` must be omitted.

enum_variant    := IDENTIFIER
                 | IDENTIFIER '=' INT_LITERAL
```

Variants are accessed via `EnumName.VariantName` — never bare names.

```luc
pub enum Direction { North  South  East  West }

pub enum ShaderStage {
    Vertex   = 0x01
    Fragment = 0x02
    Compute  = 0x04
}
```

---

## `type` Keyword

```
type_decl       := [ visibility_mod ] 'type' IDENTIFIER [ generic_params ]
                   '=' type_alias_rhs

type_alias_rhs  := type    -- any valid type expression

> [!NOTE]
> **Local usage:** Type aliases can be declared inside any block. When local, `visibility_mod` must be omitted.
```

A `type` alias introduces a new name for an existing shape — it does not
create a new distinct type. For a distinct nominal type use `struct`.

Since qualifiers are part of the function type grammar, type aliases can carry
them:

```luc
type ID           = int
type AsyncOp      = ~async    (a int) -> int
type NullableOp   = ~nullable (a int) -> int
type Parser       = (src string) -> int, string
type Step         = (a int)(b string) -> int
type ParallelBody = ~parallel (item int)

-- Generic type alias
type Transform<T> = (v T) -> T                -- T can be any type

-- nullable alias
type Transform    = (v Vec2) -> Vec2
```

---

## Trait

```
trait_decl      := [ visibility_mod ] 'trait' IDENTIFIER [ generic_params ]
                   '{' { trait_method } '}'

trait_method    := IDENTIFIER [ qualifier_list ] param_group { param_group }
                   [ '->' return_list ]
                   -- signature only — no '=' and no body
```

Rules:
- Traits can be top-level or local. When local, `visibility_mod` must be omitted.
- No field declarations — method signatures only
- No default implementations
- A struct satisfies a trait by declaring `impl StructName : TraitName { ... }`
- The impl block must provide every method in the trait
- No function overloading — each method name must be unique

```luc
pub trait Drawable {
    draw   ()
    bounds () -> Rect
}

pub trait Comparable<T> {
    compareTo (other T) -> int
}

pub trait Hashable {
    hash   () -> uint64
    equals (other any) -> bool
}

pub trait AsyncFetcher {
    fetch ~async (url string) -> string
}
```

---

## Impl Block

```
impl_decl       := [ visibility_mod ] 'impl' IDENTIFIER [ generic_params ]
                   [ ':' trait_ref ] '{' { method_decl } '}'

visibility_mod  := 'pub' | 'export'

trait_ref       := IDENTIFIER [ generic_args ]

method_decl     := IDENTIFIER [ qualifier_list ] param_group { param_group }
                   [ '->' return_list ] '=' func_body
```

Rules:
- `impl` must be in the same file (and if local, the same block) as the struct declaration
- `impl` must appear after the struct declaration
- Multiple impl blocks for the same struct merge at semantic time
- Duplicate method names across merged blocks are a semantic error
- `export impl` — methods callable externally
- `pub impl` — methods callable within the package
- `impl` — methods callable only within the file

```luc
pub impl Vec2 {
    length () -> float = {
        return #sqrt(x*x + y*y)
    }

    dot (other Vec2) -> float = {
        return x*other.x + y*other.y
    }

    scale (factor float) -> Vec2 = {
        return Vec2 { x = x * factor  y = y * factor }
    }
}

export impl Vec2 {
    normalize () -> Vec2 = {
        let len float = Vec2:length()
        return Vec2 { x = x / len  y = y / len }
    }
}

impl Vec2 {
    -- private helper, file-only
    isZero () -> bool = {
        return x == 0.0 and y == 0.0
    }
}

-- trait conformance
pub impl Circle : Drawable {
    draw   ()         = { ... }
    bounds () -> Rect = { ... }
}

-- async method
pub impl HttpClient {
    fetch ~async (url string) -> string = {
        return await httpGet(url)
    }
}

-- generic impl
pub impl Scene<T : Drawable> {
    drawAll () = {
        for obj in objects { obj.draw() }
    }
}
```

---

## From Declaration

```
from_block          := [ visibility_mod ] 'from' IDENTIFIER [ generic_params ] '{' { from_entry } '}'

from_entry          := param_group { param_group } '->' type '=' func_body
                   -- source param(s), target type name, body
                   -- target type name must match the enclosing from target

> [!NOTE]
> **Local usage:** From blocks can be declared inside any block. When local, `visibility_mod` must be omitted.
```

Rules:
A from block defines implicit conversions from a source type (described by the parameter groups) to a target struct type. Each entry contains:
- One or more parameter groups (currying allowed) – the source value(s).
- The arrow -> (mandatory).
- A single return type – must be the struct type named in the enclosing from declaration.
- `=` followed by the conversion body (a block or expression).

The body may return a value of the target type, typically via a struct literal or a call to another conversion.

### Implicit casting contexts

When a `from` declaration exists for target `T` accepting source `S`, the
compiler automatically desugars in these contexts:

1. **Variable declaration** — `let m Minutes = rawSecs`
2. **Function arguments** — `doubleKm(d)` where `d` is `Meters`
3. **Function return** — assigning `Celsius` return to `Fahrenheit` variable
4. **Assignment** — `currentTemp = newReading`

```luc
export from Fahrenheit {
    (c Celsius) Fahrenheit = {
        return Fahrenheit { value = c.value * 9.0 / 5.0 + 32.0 }
    }
    (k Kelvin) Fahrenheit = {
        return Fahrenheit { value = (k.value - 273.15) * 9.0 / 5.0 + 32.0 }
    }
}

pub from Kelvin {
    (c Celsius) Kelvin = {
        return Kelvin { value = c.value + 273.15 }
    }
}

-- Generic from block
struct Wrapper<T> { value T }

from Wrapper<T> {
    (val T) Wrapper<T> = {
        return Wrapper<T> { value = val }
    }
}

-- call site
let boiling Celsius    = Celsius { value = 100.0 }
let hot     Fahrenheit = Fahrenheit(boiling)

-- works as a pipeline step
boiling |> Fahrenheit |> io.printl
```

---

## Pipeline Operator `|>`

`|>` executes a chain left-to-right at runtime.

```
pipeline_expr   := pipeline_seed { '|>' pipeline_step }

pipeline_seed   := expr

pipeline_step   := IDENTIFIER
                 | IDENTIFIER ':' IDENTIFIER           -- impl method: Vec2:normalize
                 | IDENTIFIER '.' IDENTIFIER           -- non-nullable data field
                 | IDENTIFIER '(' arg_list ')' '!'     -- argument pack
                 | anon_func
```

| Step form      | What `\|>` does                 | Nullability                |
| -------------- | ------------------------------- | -------------------------- |
| `fn`           | calls `fn(upstream)`            | must be non-nullable       |
| `Type:method`  | calls `method(upstream)`        | always safe                |
| `struct.field` | calls function stored in field  | field must be non-nullable |
| `fn(args)!`    | calls `fn(upstream, args...)`   | must be non-nullable       |
| `anon_func`    | calls with upstream as argument | always safe                |

### The `!` argument pack annotation

> [!NOTE]
> The `!` is **forbidden** for anonymous functions.

`fn(args)!` is not a function call — the `!` marks an intentionally incomplete
argument list. The upstream value is injected as the first argument when `|>` fires.

```luc
let scale (v Vec2, factor float) -> Vec2 = { ... }

v |> scale(2.0)!     -- calls scale(v, 2.0)
v |> scale(2.0)      -- ERROR: looks like a complete call, no place for upstream
```

### Data fields as pipeline steps

A struct data field of function type can be a pipeline step only if its type
is non-nullable:

```luc
struct Processor {
    transform  (v Vec2) -> Vec2          -- non-nullable: safe as step
    onComplete ~nullable ()              -- nullable: must guard first
}

let p Processor = Processor { transform = Vec2:normalize }

v |> p.transform               -- OK
v |> p.onComplete              -- ERROR: onComplete is ~nullable

if p.onComplete != nil {
    p.onComplete()             -- OK: guarded
}
```

### `|>` vs `+>` — key difference

|                 | `\| >` pipeline                  | `+>` composition          |
| --------------- | -------------------------------- | ------------------------- |
| When            | runtime                          | compile time              |
| Seed            | required                         | none                      |
| Control flow    | full access                      | none                      |
| Type strictness | relaxed — step may ignore result | strict — types must chain |
| Result          | executes now                     | produces a new function   |

---

## Composition Operator `+>`

`+>` wires functions together at compile time without executing them. The output
type of the left must exactly match the input type of the right.

```
compose_expr    := pipeline_expr { '+>' compose_operand }

compose_operand := IDENTIFIER
                 | IDENTIFIER ':' IDENTIFIER    -- impl method
                 | IDENTIFIER '.' IDENTIFIER    -- non-nullable data field only
```

```luc
let f (a int)    -> string = { ... }
let g (s string) -> bool   = { ... }

let h   = f +> g    -- valid: f returns string, g takes string
let bad = g +> f    -- ERROR: g returns bool, f takes int

-- compose two or more
let process = validate +> transform +> render

-- result is a plain function — qualifier lives on the binding
let pipeline ~async = fetchData +> processData

-- nullable guard required before composing
if transform != nil {
    let h = transform +> render
}
```

Generic functions must be explicitly instantiated before composing — type
inference across `+>` is not supported:

```luc
let doubleInt   (x int) -> int    = { ... }
let intToString (x int) -> string = { ... }

let process = doubleInt +> intToString    -- valid: both concrete
```

---

## Expressions

```
expr            := assign_expr

assign_expr     := compose_expr [ assign_op assign_expr ]

assign_op       := '=' | '+=' | '-=' | '*=' | '/=' | '^=' | '%='
                 | '&&=' | '||=' | '~^=' | '<<=' | '>>='

compose_expr    := pipeline_expr { '+>' compose_operand }

pipeline_expr   := pipeline_seed { '|>' pipeline_step }

pipeline_seed   := logical_expr

logical_expr    := compare_expr { ( 'and' | 'or' ) compare_expr }

compare_expr    := bitwise_expr [ compare_op bitwise_expr ]
                 | bitwise_expr 'is' type

compare_op      := '==' | '!=' | '<' | '>' | '<=' | '>=' | '==='

bitwise_expr    := shift_expr { ( '&&' | '||' | '~^' ) shift_expr }

shift_expr      := add_expr { ( '<<' | '>>' ) add_expr }

add_expr        := mul_expr { ( '+' | '-' ) mul_expr }

mul_expr        := pow_expr { ( '*' | '/' | '%' ) pow_expr }

pow_expr        := unary_expr [ '^' pow_expr ]      -- right-associative

unary_expr      := ( 'not' | '-' | '~~' | '&' ) unary_expr
                   -- '~~' is bitwise NOT on integers
                 | postfix_expr

postfix_expr    := primary_expr { postfix_op }

postfix_op      := '.' IDENTIFIER
                 | ':' IDENTIFIER
                 | '?.' IDENTIFIER
                 | '??' expr
                 | '[' expr ']'
                 | '[' expr '..'  expr ']'
                 | '[' expr '..<' expr ']'
                 | '(' [ arg_list ] ')'
                 | '(' [ arg_list ] ')' '!'
                 | generic_args '(' [ arg_list ] ')'

primary_expr    := literal
                 | IDENTIFIER
                 | struct_literal
                 | '(' expr ')'
                 | anon_func
                 | match_expr
                 | if_expr
                 | array_literal
                 | 'nil' | 'true' | 'false'
                 | await_expr
                 | range_expr

await_expr      := 'await' expr
                   -- expr must be a call to a ~async-qualified function
                   -- valid only inside a ~async function body

range_expr      := expr range_op expr [ '..' expr ]

range_op        := '..'     -- inclusive end
                 | '..<'    -- exclusive end

arg_list        := expr { [','] expr }
```

---

## Async / Await

`~async` marks a function binding as asynchronous. `await` suspends the current
function until the awaited call resolves.

```luc
let fetch ~async (url string) -> string = {
    return await httpGet(url)    -- httpGet must also be ~async
}

-- calling an ~async function requires await
let result string = await fetch("https://api.example.com")

-- await is only valid inside a ~async body
let bad (url string) -> string = {
    return await fetch(url)    -- ERROR: not inside ~async body
}
```

**Rules:**

- `await` is only valid inside a function body whose binding carries `~async`
- `await expr` — `expr` must resolve to a call to a `~async`-qualified function
- An `~async` function may freely call non-async functions
- `await` is not valid inside a `~parallel` body function

---

## Arrays

Three distinct kinds:

| Kind    | Syntax | Memory           | Growable |
| ------- | ------ | ---------------- | -------- |
| Fixed   | `[N]T` | stack/inline     | ❌        |
| Slice   | `[]T`  | fat pointer view | ❌        |
| Dynamic | `[*]T` | heap-owned       | ✅        |

```luc
let rgba [4]float  = [1.0, 0.5, 0.0, 1.0]
let nums [*]int    = [10, 20, 30, 40, 50]
let view []int     = nums[1..3]     -- elements at index 1, 2, 3 (inclusive)
let excl []int     = nums[1..<3]    -- elements at index 1, 2 (exclusive end)
```

> [!NOTE]
> - **Array elements are not nullable by default.** To allow nil elements use `T?`:` let nums [*]int? = [1, nil, 3]`
> - **Out of bounds:** Fixed and slice arrays panic at runtime. Dynamic arrays return nil for out-of-bounds index access.

### Slice range rules

| Operator | End bound | Example on `[10,20,30,40,50]`              |
| -------- | --------- | ------------------------------------------ |
| `..`     | inclusive | `nums[1..3]` → `[20, 30, 40]` (3 elements) |
| `..<`    | exclusive | `nums[1..<3]` → `[20, 30]` (2 elements)    |

### Built-in methods

**All kinds (`[N]T`, `[]T`, `[*]T`):**

| Method       | Returns | Description          |
| ------------ | ------- | -------------------- |
| `.len()`     | `int`   | number of elements   |
| `.isEmpty()` | `bool`  | true if `len() == 0` |
| `[i]`        | `T`     | element at index     |
| `[i..j]`     | `[]T`   | inclusive slice      |
| `[i..<j]`    | `[]T`   | exclusive slice      |

**Slice and dynamic (`[]T`, `[*]T`):**

| Method     | Returns | Description        |
| ---------- | ------- | ------------------ |
| `.cap()`   | `int`   | allocated capacity |
| `.first()` | `T`     | first element      |
| `.last()`  | `T`     | last element       |

**Dynamic only (`[*]T`):**

| Method                | Returns | Description                           |
| --------------------- | ------- | ------------------------------------- |
| `.push(v T)`          | —       | append to end                         |
| `.pop()`              | `T`     | remove and return last                |
| `.insert(i int, v T)` | —       | insert at index                       |
| `.remove(i int)`      | `T`     | remove at index                       |
| `.clear()`            | —       | remove all elements, free heap buffer |
| `.reserve(n int)`     | —       | pre-allocate capacity                 |

`+` is defined on `[]T` and `[*]T` — produces a new array containing all
elements of both operands.

---

### Arrays of Function Types

Arrays of any type, including function types, are fully supported.
```luc
-- slice of functions that take an int and return a bool
let predicates [] (int) -> bool = [isEven, isPositive, isPrime]

-- dynamic array of async functions
let asyncTasks [*] ~async (url string) -> string = [fetchUser, fetchPosts]

-- fixed array of curried functions
let curries [2] (int)(int) -> int = [add, mul]
```

#### Allowed Operations

| Operation            | Example                         | Notes                                                                      |
| -------------------- | ------------------------------- | -------------------------------------------------------------------------- |
| Store function       | `handlers[0] = validate`        | The function type must match the array’s element type exactly.             |
| Call through index   | `let result = handlers[i](arg)` | The index expression must be an integer; the call follows normal rules.    |
| Pass as argument     | `applyAll(handlers, data)`      | The array itself is passed by value (owned) or by reference (`&[]T`).      |
| Return from function | `return getCallbacks()`         | Ownership follows array semantics (deep copy for dynamic, view for slice). |

> [!WARNING] Restrictions
>These are limitation about array of function

1. **No equality** – Function types are not comparable (`==`, `!=`). Consequently, arrays of functions are also not comparable. This is enforced at the type level.

```luc
let a [] (int) -> int = [f]
let b [] (int) -> int = [f]
if a == b { ... }   -- ERROR: cannot compare arrays of function type
```

2. Qualifiers are not part of type identity – Storing a ~async function in an array does not make the array element ~async. The qualifier belongs to the binding at the call site, not to the value. The stored value is a plain function pointer or closure.

```luc
let asyncFn ~async (x int) -> int = { ... }
let arr [] (int) -> int = [asyncFn]   -- OK: qualifier ignored for storage

-- Call site must still obey qualifier rules
let result = await arr[0](5)   -- OK: caller uses await because the stored function is ~async
let result = arr[0](5)         -- ERROR: ~async called without await
```

3. Closure capture – If a stored function captures variables (a closure), the array holds a reference to the closure’s environment. This may prevent memory from being freed until the array is cleared or goes out of scope. Use .clear() on dynamic arrays to release closures early if needed.

4. No generic specialisation – The element type of an array is a concrete function signature. Generic functions cannot be stored directly unless instantiated:

```luc
let idInt   = identity<int>   -- instantiate to (int) -> int
let arr [] (int) -> int = [idInt]   -- OK
let arr [] (T) -> T = [identity]    -- ERROR: generic function without type arguments
```

> [!TIP] Arrays of function types enable:  
> - Dispatch tables – Replace switch/match with indexed function lookup.    
> - Callback lists – Event handlers, middleware chains, plugin systems.  
> - Higher‑order collections – Store partially applied functions, curried functions, or stateful closures.
> - Interpreters & DSLs – Represent operations as functions in a data structure.


## Statements

```
stmt            := { attribute } actual_decl
                 | multi_assign         -- declaration with let/const
                 | multi_assign_stmt    -- reassignment to existing variables
                 | assign_stmt
                 | if_stmt
                 | switch_stmt
                 | for_stmt
                 | while_stmt
                 | do_while_stmt
                 | return_stmt
                 | break_stmt
                 | continue_stmt
                 | expr_stmt

block           := '{' { stmt } '}'

expr_stmt       := expr
assign_stmt     := expr assign_op expr
```
> [!WARNING]
> **Visibility inside blocks:**  
> `pub` and `export` are **not allowed** on any local declaration – they are top‑level only. The parser emits an error if they appear inside a block.

> **Attributes on local declarations:**  
> Attributes (`@inline`, `@deprecated`, `@unroll`, etc.) **are allowed** on local declarations (type, struct, enum, impl, from, var, func). They are attached to the declaration node and may be used by the semantic pass.

### Examples of valid local declarations

```luc
let compute () -> int = {
    @deprecated("use newVec")
    type Vec2 = struct { x float, y float }

    @inline
    let add (a int, b int) -> int = { return a + b }

    struct Point { x int, y int }

    enum Color { Red, Green, Blue }

    impl Point {
        length () -> float = { return #sqrt(x*x + y*y) }
    }

    from Point {
        (v float) -> Point = { return Point { x = v, y = v } }
    }

    let p Point = Point(5.0)
    p:length()
}


### If / Else — Statement Form

```
if_stmt         := 'if' expr block [ 'else' ( if_stmt | block ) ]
```

`else` is optional. Type narrowing applies inside `thenBranch` when condition
is an `is` expression.

```luc
if score >= 90 {
    io.printl("A")
} else if score >= 80 {
    io.printl("B")
} else {
    io.printl("F")
}
```

### If Expression — Inline Form

```
if_expr         := 'if' expr '??' expr 'else' expr
```

`else` is **required** in expression form. Both branches must produce
compatible types. `??` is the separator between condition and then-branch —
it is not the null-coalescing operator in this context.

```luc
let grade string = if score >= 60 ?? "pass" else "fail"

-- chained (right-associative)
let label string = if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive"
```

### Match

`match` is expression-oriented — it produces a value. `default` is required
and must be last. Arm bodies are expressions only — no blocks, no `return`.

```
match_expr      := 'match' expr '{' { match_arm } default_arm '}'

match_arm       := pattern_list [ guard ] '=>' arm_body

default_arm     := 'default' '=>' arm_body

arm_body        := expr [ ',' expr ]
                   -- first expr: primary value
                   -- second expr: optional secondary value

pattern_list    := pattern { ',' pattern }

guard           := 'if' logical_expr

pattern         := literal
                 | range_expr
                 | IDENTIFIER
                 | IDENTIFIER 'is' type
                 | WILDCARD
                 | struct_pattern

struct_pattern  := IDENTIFIER '{' { field_pattern } '}'
field_pattern   := IDENTIFIER [ ':' pattern ]
```

**Secondary value rules:**

| Situation                       | Rule                                    |
| ------------------------------- | --------------------------------------- |
| No arm supplies secondary       | match produces one value                |
| Every arm supplies secondary    | second variable is non-nullable         |
| Only some arms supply secondary | second variable must be nullable (`T?`) |

```luc
-- single value
let label string = match status {
    200      => "ok"
    404      => "not found"
    default  => "unknown"
}

-- multiple values per arm
let category string = match code {
    200, 201, 202 => "success"
    400, 401      => "client error"
    default       => "other"
}

-- range pattern
let severity string = match damage {
    0       => "none"
    1..10   => "light"
    11..<50 => "moderate"
    default => "critical"
}

-- bind with guard
let label string = match score {
    n if n < 0  => "invalid: " + string(n)
    n if n < 50 => "fail: "    + string(n)
    n           => "pass: "    + string(n)
    default     => "unknown"
}

-- struct destructuring
let desc string = match point {
    Vec2 { x: 0.0, y: 0.0 } => "origin"
    Vec2 { x, y }            => "at " + string(x) + ", " + string(y)
    default                  => "unknown"
}

-- secondary value (some arms supply it, second variable is nullable)
let label string
let detail string?
label, detail = match status {
    200     => "ok",        "request succeeded"
    404     => "not found"   -- detail is implicitly nil
    default => "unknown"
}
```

### Switch

`switch` is statement-oriented — dispatches on a value, runs blocks, produces
no value.

```
switch_stmt     := 'switch' expr '{' { case_clause } [ default_clause ] '}'

case_clause     := 'case' case_value { ',' case_value } ':' block

case_value      := expr | range_expr

default_clause  := 'default' ':' block
```

No fallthrough — each case is isolated.

```luc
switch code {
    case 200, 201: { io.printl("ok") }
    case 1..10:    { io.printl("light") }
    case 11..<50:  { io.printl("moderate") }
    default:       { io.printl("other") }
}
```

### For Loop

```
for_stmt        := 'for' IDENTIFIER [ type ] 'in' ( range_iter | expression ) block

range_iter      := expression range_op expression [ '..' expression ]
                   -- start range_op end [ .. step ]
                   -- default step is 1 if omitted
                   -- type defaults to 'int' for integer boundaries if omitted
```

```luc
for i in 0..10 { io.printl(string(i)) }       -- 0 through 10 inclusive
for i in 0..<10 { io.printl(string(i)) }      -- 0 through 9
for item in items { process(item) }
for i int in 0..10..2 { io.printl(string(i)) } -- step of 2
```

### While / Do-While

```
while_stmt      := 'while' expr block

do_while_stmt   := 'do' block 'while' expr
```

### Return / Break / Continue

```
return_stmt     := 'return' [ expr ]
break_stmt      := 'break'
continue_stmt   := 'continue'
```

---

## Comparison Rules

### `==` — Value Equality

Valid for: primitives, enums, nullable types.

Not valid for:
- Struct types — implement `Equatable<T>` and use `:equals()` instead
- Function types — function bodies are incomparable
- Array types — use collection library comparison

### `===` — Reference Equality

Checks same memory address. Valid for `&T`, structs, nullable reference types.

### `is` — Type Identity

Checks the type of a value. Narrows the type inside the enclosing block
(statement form only). Nullable and non-nullable are distinct types.

```luc
let x int? = 5
x is int    -- false: int? is NOT int
x is int?   -- true
x == 5      -- true: value comparison unwraps nullable automatically
```

**Chained comparisons are not allowed:**

```luc
if 0 < x and x < 10 { ... }    -- correct
if 0 < x < 10 { ... }          -- ERROR: chained comparison
```

---

## Logical Operators

`and` and `or` short-circuit. Both operands must be `bool` or nullable:

```luc
if getUser() != nil and validate(getUser()) { ... }   -- short-circuit: right only if left non-nil
if cache:has(key) or expensiveLoad(key) { ... }        -- short-circuit: right only if left false
```

`not` operates on `bool` and nullable types:

```luc
if not isValid { ... }
if not x { ... }    -- x is nullable: nil treated as false, not flips to true
```

---

## Bitwise Operators

Integer types only. `&&` and `||` are bitwise AND/OR (not logical — those use
`and`/`or` keywords). This avoids ambiguity with `&` (reference operator).

| Operator | Name                |
| -------- | ------------------- |
| `&&`     | bitwise AND         |
| `\|\|`   | bitwise OR          |
| `~^`     | bitwise XOR         |
| `~~`     | bitwise NOT (unary) |
| `<<`     | left shift          |
| `>>`     | right shift         |

```luc
let flags  uint32 = 0xFF00
let mask   uint32 = 0x0F0F
let result uint32 = flags && mask     -- 0x0F00
let merged uint32 = flags || mask     -- 0xFF0F
let inv    uint32 = ~~flags           -- bitwise NOT
let shifted uint32 = 1 << 4           -- 16
```

---

## Operator Precedence (High → Low)

| Level       | Operators                                                         | Associativity |
| ----------- | ----------------------------------------------------------------- | ------------- |
| 1 (highest) | `()` `.` `:` `?.` `[…]` `!` calls                                 | left          |
| 2           | unary `-` `not` `~~` `&`                                          | right         |
| 3           | `^` (power)                                                       | right         |
| 4           | `*` `/` `%`                                                       | left          |
| 5           | `+` `-`                                                           | left          |
| 6           | `<<` `>>`                                                         | left          |
| 7           | `&&` `\|\|` `~^`                                                  | left          |
| 8           | `==` `!=` `<` `>` `<=` `>=` `===`                                 | left          |
| 9           | `and`                                                             | left          |
| 10          | `or`                                                              | left          |
| 11          | `??`                                                              | right         |
| 12          | `                                                                 | >` (pipeline) | left |
| 13          | `+>` (composition)                                                | left          |
| 14          | `=` `+=` `-=` `*=` `/=` `^=` `%=` `&&=` `\|\|=` `~^=` `<<=` `>>=` | right         |
| 15 (lowest) | `if ?? else`                                                      | right         |

> [!NOTE]
> **`if_expr` precedence:** `if cond ?? thenExpr else elseExpr` begins with the `if` keyword. The `??` here is a fixed syntactic separator in the if-expression production, not the null-coalescing infix operator (level 11). The `else` clause is right-associative at the lowest precedence — chained `if ?? ... else if ?? ... else ...` forms bind correctly.

---

## `@` and `#` — Compiler Directives

Two distinct prefixes, distinguished by position and symbol:

| Prefix                  | Position               | Purpose                           |
| ----------------------- | ---------------------- | --------------------------------- |
| `@name` / `@name(args)` | Before a declaration   | Attach metadata (attribute)       |
| `#name(args)`           | In expression position | Compiler-builtin call (intrinsic) |

### Attributes (`@`)

```
attribute       := '@' IDENTIFIER [ '(' attr_arg_list ')' ]

attr_arg_list   := attr_arg { ',' attr_arg }

attr_arg        := STRING_LITERAL | INT_LITERAL | HEX_LITERAL | 'true' | 'false' | IDENTIFIER
```

Attribute arguments are intentionally limited to compile-time literals and
type identifiers. Runtime expressions are not valid inside attribute arguments.

#### Known Attributes

| Attribute                | Valid on                | Purpose                                   |
| ------------------------ | ----------------------- | ----------------------------------------- |
| `@extern("sym")`         | `let`, `const` func/var | Bind to C/OS/Vulkan symbol                |
| `@extern("sym", "conv")` | `let`, `const` func/var | With explicit calling convention          |
| `@inline`                | func                    | Suggest always inline                     |
| `@noinline`              | func                    | Prevent inlining                          |
| `@packed`                | `struct`                | Remove padding — all fields byte-adjacent |
| `@deprecated("msg")`     | func, var, struct       | Emit warning at every use site            |
| `@aot`                   | `main` only             | Ahead-of-time compilation                 |
| `@jit`                   | `main` only             | JIT compilation                           |

`@inline` and `@noinline` are mutually exclusive on the same declaration.
`@aot` and `@jit` are mutually exclusive on the same declaration.

#### `@extern` Rules

- Requires `const`, not `let` — the linker resolves the symbol permanently
- Functions must have no body
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

| Intrinsic     | Returns  | Notes                                              |
| ------------- | -------- | -------------------------------------------------- |
| `#sizeof(T)`  | `uint64` | Byte size of type T — compile-time constant        |
| `#alignof(T)` | `uint64` | Alignment requirement of T — compile-time constant |

```luc
let size   uint64 = #sizeof(Vertex)
let align  uint64 = #alignof(Vec2)
```

---

#### Floating-point math

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

```luc
let hyp       float = #sqrt(x*x + y*y)
let rounded   float = #round(value)
let maxVal    int   = #min(a, b)
```

---

#### Bit manipulation (integer types only)

| Intrinsic      | Args    | Returns | Notes                           |
| -------------- | ------- | ------- | ------------------------------- |
| `#clz(x)`      | integer | same    | Count leading zero bits         |
| `#ctz(x)`      | integer | same    | Count trailing zero bits        |
| `#popcount(x)` | integer | same    | Count set (1) bits              |
| `#bswap(x)`    | integer | same    | Reverse byte order (endianness) |

```luc
let leading   uint32 = #clz(flags)
let trailing  uint32 = #ctz(flags)
let bits      uint32 = #popcount(mask)
let swapped   uint32 = #bswap(networkOrder)
```

---

#### Memory operations

| Intrinsic                 | Args               | Returns | Notes                       |
| ------------------------- | ------------------ | ------- | --------------------------- |
| `#memcpy(dst, src, len)`  | ptr, ptr, uint64   | void    | Copy bytes, no overlap      |
| `#memmove(dst, src, len)` | ptr, ptr, uint64   | void    | Copy bytes, handles overlap |
| `#memset(dst, val, len)`  | ptr, ubyte, uint64 | void    | Fill bytes with value       |

All memory intrinsics operate on raw pointers (`*T`) and are only valid inside
`@extern`-decorated functions or other intrinsic calls.

```luc
#memcpy(dest, src, #sizeof(Buffer))
#memset(ptr, 0, size)
```

---

#### Pointer operations (The Sealed Conduit boundary)

| Intrinsic            | Args       | Returns | Notes                                 |
| -------------------- | ---------- | ------- | ------------------------------------- |
| `#ptrToRef(ptr)`     | type, `*T` | `&T`    | Assert valid, cross to safe reference |
| `#refToPtr(ref)`     | `&T`       | `*T`    | Convert reference to raw pointer      |
| `#ptrOffset(ptr, n)` | `*T`, int  | `*T`    | Pointer arithmetic (element offset)   |
| `#ptrDiff(p1, p2)`   | `*T`, `*T` | `int64` | Distance between pointers in elements |

These intrinsics are the only way to cross the sealed conduit boundary or
perform pointer arithmetic.

```luc
let buf *uint8 = malloc(1024)
let ref &uint8 = #ptrToRef(buf)
ref = 0xFF

let next *uint8 = #ptrOffset(buf, 1)
let distance int64 = #ptrDiff(next, buf)
```

---

#### Unsafe / Bit reinterpretation

| Intrinsic        | Args        | Returns | Notes                                             |
| ---------------- | ----------- | ------- | ------------------------------------------------- |
| `#bitcast(T, x)` | type, value | `T`     | Reinterpret bits of x as type T; sizes must match |

Valid only inside `@extern`-decorated functions or when the compiler flag
`--unsafe` is enabled.

```luc
let bits uint32 = 0x3F800000
let f   float32 = #bitcast(float32, bits)   -- 1.0
```

---

## Choice and Fallback Operators

```
fallback_expr   := expr '??' expr
                   -- LHS is T (non-nil): result is T
                   -- LHS is nil or Error: result is RHS

catch_step      := expr '|>' 'catch' '(' identifier ')' block
                   -- upstream is T: block skipped, T passed along
                   -- upstream is Error: identifier bound to Error, block executes
```

---

## Literals

```
literal         := INT_LITERAL
                 | FLOAT_LITERAL
                 | STRING_LITERAL
                 | RAW_STRING_LITERAL      -- r"raw\nno escaping"
                 | CHAR_LITERAL
                 | HEX_LITERAL             -- 0xFF
                 | BINARY_LITERAL          -- 0b1010

RAW_STRING_LITERAL := 'r' [ '#'+ ] '"' { any character } '"' [ '#'+ ]
                   -- the number of '#' before the opening quote must match
                   -- the number after the closing quote (e.g., r#"..."#, r##"..."##)
                   -- no escape processing; the literal content does not close the string
                   -- when it contains a quote that would otherwise be ambiguous.
```

Raw string literals use r followed by zero or more # characters, then a double quote, the literal content, a double quote, and the same number of # characters.
The content is taken verbatim – no escape sequences are processed, and the delimiter pairs ("# / #") allow the string to contain arbitrary quotes and backslashes without ambiguity.

```luc
let a = r"hello"                     -- simple, no quotes inside
let b = r#"He said "Hello""#         -- contains a double quote
let c = r##"She said "#Hello"#"##    -- contains both " and "# sequences
```

### String Escape Sequences

| Sequence     | Meaning                          |
| ------------ | -------------------------------- |
| `\n`         | newline (LF)                     |
| `\t`         | tab                              |
| `\r`         | carriage return                  |
| `\"`         | literal double quote             |
| `\\`         | literal backslash                |
| `\0`         | null character                   |
| `\xFF`       | hex byte value                   |
| `\u0041`     | Unicode codepoint (4 hex digits) |
| `\U0001F600` | Unicode codepoint (8 hex digits) |

Raw string literals `r"..."` — no escape processing. Backslashes are literal.

```luc
let a string = "hello\nworld"
let b string = r"^\d{3}-\d{4}$"             -- regex: backslashes literal
let c string = r"C:\Users\luc\config.txt"   -- Windows path
```

### Char Literals

```
CHAR_LITERAL    := '\'' ( char | escape_seq ) '\''
```

```luc
let a char = 'A'
let b char = '\n'
let c char = '\x41'     -- hex — same as 'A'
let d char = '\u0041'   -- Unicode — same as 'A'
```

---

## Comments

```luc
-- single-line comment

/- block comment
   spans multiple lines
-/

/--
 - document comment — attached to the immediately following declaration
 - each continuation line starts with ' -'
 - Markdown content is supported
--/
```

### Comment Grammar

```
line_comment    := '--' { any char except newline }

block_comment   := '/-' { any char } '-/'
                   -- may span multiple lines, nesting NOT supported

doc_comment     := '/--' { ' -' line_content } '--/'
                   -- must immediately precede a top-level declaration
```

### Lexer Disambiguation

`/-` and `/--` both start with `/`:

- `/` followed by `--` → doc comment `/--`
- `/` followed by `-` → block comment `/-`
- `/` alone → `DIV` token

### Doc Comment Attachment Rules

1. Stacked `--` lines immediately above → attach as stacked doc
2. `--` on the same line → trailing doc
3. `/-- --/` immediately above → block doc
4. Stacked above + trailing on same line → stacked wins, trailing ignored
5. Blank line between comment and declaration → comment is floating, not attached
6. `--` lines above a `/-- --/` block → block attaches; `--` lines above it are floating

```luc
-- normalizes the vector in place
-- only call after the vector has been validated
let normalize (v Vec2) -> Vec2 = { ... }    -- stacked attaches

let maxVertices int = 65536   -- Vulkan hard limit   -- trailing attaches

/--
 - Computes the dot product of two vectors.
 -
 - Returns `|a| * |b| * cos(angle)`.
--/
pub impl Vec2 {
    dot (other Vec2) -> float = { ... }    -- block attaches
}
```

---

## Keywords (Reserved)

```
pub export package use as impl trait type from
let const struct enum
await
bool byte short int long ubyte ushort uint ulong
int8 int16 int32 int64 uint8 uint16 uint32 uint64
float double decimal string char any nil
if else match switch case default is
while for in do return break continue
and or not true false
```
