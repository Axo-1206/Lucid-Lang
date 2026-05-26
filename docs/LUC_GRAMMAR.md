# Luc — Grammar Reference

> [!TIP]
> Scope of this file
> Formal grammar rules for the Luc parser. Code examples are in `LUC_EXAMPLES.md`. Project identity is in `LUC_PROJECT_OVERVIEW.md`.

---

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

---

## Separators

Commas `,` and semicolons `;` are **optional** throughout — Luc uses newlines and block structure to delimit constructs (Go / scripting style). The parser must accept them but never require them.

---

## Keywords (Reserved)

```
pub export package use as impl trait type from
let const struct enum await resolve
bool byte short int long ubyte ushort uint ulong
int8 int16 int32 int64 uint8 uint16 uint32 uint64
float double decimal string char any nil
if else match switch case default is
while for in do return break continue
and or not true false
```

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

### Entry Point (`main`)

```luc
export const main () -> int = {
    return 0
}

-- with command-line arguments
export const main (args string[]) -> int = {
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

Every `.luc` file is a module. The module's identity is its file path relative to the package root. Modules are flat; nesting is not supported.

### Visibility — Three Tiers

| Keyword  | Scope       | Access                                                     |
| -------- | ----------- | ---------------------------------------------------------- |
| (none)   | **File**    | Only visible within the same `.luc` file                   |
| `pub`    | **Package** | Visible to all files sharing the same `package` identifier |
| `export` | **World**   | Visible to external consumers of the package               |

- `pub` and `export` are top-level modifiers only — **illegal** inside blocks or function bodies. Declarations inside blocks are implicitly private to that block.
- `pub impl` / `export impl` controls method accessibility at the block level when used at top-level.
- `pub from` / `export from` controls converter accessibility at the block level when used at top-level.
- If a package has zero `export` declarations it is a private package.
- Most declarations (`struct`, `enum`, `type`, `trait`, `impl`, `from`, `let`, `const`, `func`) are allowed in local scopes.

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
> `use` cannot be declared inside any block.

### Re-Exports

```luc
-- math/api.luc
package math

export use math.vec2           -- re-export all pub items from vec2
export use math.matrix.Mat2    -- granular re-export of a single type
```

---

## Types

```
type            := base_type [ generic_args ] [ '?' ] [ result_suffix ]
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
-- With postfix array syntax, '?' after an array type attaches to the whole array unambiguously.
-- '?' is NEVER valid directly on an inline function type — use a named alias or ~nullable qualifier.
nullable_suffix := '?'

-- Result suffix: marks a type as potentially failed.
-- '?' always comes before '!' when both are present.
-- The error type after '!' is required — bare '!' with no error type means failure carries nil.
-- '!' binds to the immediately preceding type token — unambiguous for primitives, structs,
-- enums, named aliases, and postfix array types. Inline function types require a named alias.
-- Nesting '!' is forbidden — neither the success type nor the error type may itself carry '!'.
-- '?' after '!E' binds to E, not to the whole T!E — to make the whole result nullable, alias first.
result_suffix   := '!' type     -- success T, failure E  (T and E must be plain, non-! types)
                 | '!'          -- success T, failure nil

-- Reference (&T) — safe managed reference
ref_type        := '&' type

-- Raw pointer (*T) — allowed anywhere, but operations (dereference, indexing,
-- arithmetic) are forbidden. Use only for storage, nil checks, and intrinsics.
ptr_type        := '*' type

-- Array types — bracket-prefix notation: kind and element type are enclosed together.
-- This eliminates all suffix ambiguity: '?' and '!' always attach to the whole array type.
-- Inline function types ARE allowed as array element types — the closing ']' unambiguously
-- marks the end of the element type.
array_type      := '[' '_' ',' type ']'         -- slice:   [_, T]  (fat pointer view)
                 | '[' '*' ',' type ']'         -- dynamic: [*, T]  (heap-owned, growable)
                 | '[' INT_LITERAL ',' type ']' -- fixed:   [N, T]  (stack, compile-time size)

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

> [!WARNING]
> No union types
> Luc does **NOT** have union types (`T | U`). The idiomatic replacement depends on what you actually need:
>
> **For dynamic dispatch** — use `any` with `is` checks or `match` for type narrowing:
>
> ```luc
> let x any = 5
> if x is int    { ... }
> if x is string { ... }
>
> let label string = match x {
>     n is int    => "int: "    + string(n)
>     s is string => "string: " + s
>     default     => "unknown"
> }
> ```
>
> **For static safety** — model it explicitly with a tagged struct:
>
> ```luc
> enum PayloadKind { Int  Str }
>
> struct Payload {
>     kind  PayloadKind
>     asInt int?
>     asStr string?
> }
> ```
>
> **The phantom alias trick does not work.** It may be tempting to use `@phantom` to simulate a union:
>
> ```luc
> @phantom
> type Union<T> = any
>
> let x Union<int> = 5    -- looks constrained, but T is erased at runtime
> ```
>
> This is **not a real union** — `T` carries no runtime information and the compiler cannot enforce that the value inside actually conforms to it. `Union<int>` and `Union<string>` are distinct types at the type-checking level but both resolve to plain `any` at runtime. You get the appearance of a constraint with none of the safety. Use `any` directly and be honest about it.

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

-- function returning nullable function — alias or ~nullable qualifier required
type Transform = (v Vec2) -> Vec2
let f  (a int) -> Transform? = { ... }
let g  (a int) -> ~nullable (v Vec2) -> Vec2? = { ... }

-- '?' is NEVER valid directly after an inline function type as a whole.
-- Use a type alias or ~nullable qualifier instead.
-- let f (a int) -> (b int) -> int? ?   -- ERROR: use an alias
```

### `?` Binding on Array Types

With bracket-enclosed array syntax, `?` after an array type attaches to the whole array unambiguously — the `]` closes the type before `?` is encountered:

```luc
[_, int]?     -- ([_, int])?  — nullable slice of int
[*, int]?     -- ([*, int])?  — nullable dynamic array
[_, int?]     -- [_, (int?)]  — slice of nullable int
[*, int?]     -- [*, (int?)]  — dynamic array of nullable int
```

An alias is still recommended when the name adds meaning:

```luc
type IntList   = [_, int]
type UserArray = [*, User]

let arr  IntList?   = nil    -- nullable slice — the array itself may be absent
let list UserArray? = nil    -- nullable dynamic array

-- nullable array of nullable int
type MaybeIntList = [_, int?]       -- slice of nullable int
let arr MaybeIntList? = nil         -- nullable slice of nullable int
```

> [!NOTE]
> Nullable arrays are rare. If a function might return nothing, prefer returning an empty array over a nullable array. Reserve `T?` on an array alias only for cases where the absence of the array is semantically distinct from an empty array — for example, a field that has never been loaded vs a field that was loaded and found to be empty.

---

## Value and Reference Semantics

Every type is either **owned** or **borrowed**. Bare `T` = owned, `&T` = borrowed.

### Owned Types — Copied on Assignment

| Type           | Syntax                           | Storage                         | On assignment                      |
| -------------- | -------------------------------- | ------------------------------- | ---------------------------------- |
| Primitives     | `bool` `int` `float` `char` …    | stack / inline                  | full copy                          |
| Enum           | `Direction.North`                | integer (`byte`/`short`)        | full copy                          |
| Fixed array    | `T[*N]`                          | stack / inline                  | full element copy                  |
| Slice          | `T[]`                            | fat pointer (`ptr + len + cap`) | copies view header — shares buffer |
| Dynamic array  | `T[*]`                           | heap-owned buffer               | full deep copy                     |
| String         | `string`                         | heap-owned sequence             | full deep copy                     |
| Struct         | `Vec2` `Player` …                | inline / stack                  | full deep copy                     |
| Named function | `add` `myVector:normalize`       | function pointer                | pointer copy                       |
| Closure        | `add(10)` `(x int) -> int { … }` | heap-allocated env              | copies reference to env            |

### Struct Deep Copy

Struct assignment always produces a fully independent value. Owned fields are cloned, borrowed (`&T`) fields copy their reference only.

```luc
struct Player {
    score  int        -- owned: cloned
    items  string[*]  -- owned: buffer deep-copied
    world  &World     -- borrowed: reference copied, World not cloned
}

let a Player = Player { score = 10  items = ["sword"]  world = &w }
let b Player = a
-- b.score and b.items are independent; b.world shares the same World as a.world
```

### Borrowed Types — Share Without Copying

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

### Circular References

```luc
struct Node {
    value int
    next  &Node?    -- borrowed reference, nullable
}
```

### Function Values and Closures

Named functions and impl methods are plain function pointers — no captured state. Closures (partial applications, anonymous functions capturing variables) hold a heap-allocated environment. Assigning a closure copies the reference to that environment.

### `any` and Boxing

A value stored in `any` is boxed with a runtime type tag. Owned types placed into `any` are deep-copied into the box. Borrowed references (`&T`) store the reference — the box does not own the referenced value.

---

## The Sealed Conduit Model (Raw Pointers)

Raw pointers (`*T`) are sealed conduits — carry them, pass them to extern functions, check for nil, but never dereference directly.

### Allowed Operations

1. Store in a variable, struct field, or parameter
2. Pass to an `@extern` function
3. Nil check (`== nil`, `!= nil`)
4. Pass to pointer intrinsics (`#ptrToRef`, `#ptrOffset`, etc.)
5. Print the address for debugging

### Forbidden Operations (Compiler Error)

- Dereferencing: `*ptr`
- Field access: `ptr.field`
- Indexing: `ptr[i]`
- Arithmetic: `ptr + 4` — use `#ptrOffset` instead
- Assignment: `*ptr = value`

### Boundary Crossing (Intrinsics)

```luc
#ptrToRef(ptr)      -- *T → &T  (assert validity, cross to safe reference)
#refToPtr(ref)      -- &T → *T  (convert back to raw pointer)
#ptrOffset(ptr, n)  -- pointer arithmetic, returns new *T
#ptrDiff(p1, p2)    -- distance between two pointers as int64
```

```luc
@extern("malloc")
const malloc (size uint64) -> *uint8?

let buf *uint8? = malloc(1024)
if buf == nil { return 1 }

let ref &uint8 = #ptrToRef(buf)        -- cross the boundary
ref = 0xFF                             -- work with it safely

let next *uint8? = #ptrOffset(buf, 1)  -- pointer arithmetic
```

---

## Variable Declaration

Variables can be declared inside a block. Multiple declaration is forbidden at top level. Local declarations **must not** have `pub` or `export`.

```
var_decl        := [ visibility_mod ] decl_keyword IDENTIFIER type_ann [ '=' expr ]

decl_keyword    := 'let' | 'const'

type_ann        := type
```

> [!WARNING]
> No type inference
> Luc does not infer types. Every declaration **must** include an explicit type annotation. The compiler rejects any declaration without one.
>
> ```luc
> let x     = 5     -- ERROR: type annotation required
> let x int = 5     -- OK
> ```

### Declaration Semantics

| Keyword | Reassignable | Mutable in place | Value known at | Nil allowed             |
| ------- | ------------ | ---------------- | -------------- | ----------------------- |
| `let`   | ✅            | ✅                | runtime        | ✅ (if type is nullable) |
| `const` | ❌            | ❌                | compile time   | ❌                       |

`const` requires a compile-time constant initialiser: a literal, another `const` name, an enum variant, or arithmetic over those. Function calls, struct literals, and runtime values are rejected by the semantic pass.

```luc
const MAX   int   = 65536
const PI    float = 3.14159
const HALF  float = PI / 2.0
const STAGE int   = ShaderStage.Vertex

const x int  = readInput()        -- ERROR: function call is not compile-time
const v Vec2 = Vec2 { x = 0.0 }  -- ERROR: struct literal is not compile-time
```

### Non-Nullable Variables and Memory

A non-nullable variable (e.g. `int`, `Vec2`, `string`) **can never be set to nil**. This eliminates null pointer exceptions for these types. The compiler guarantees that if a non-nullable variable exists it holds a valid value.

- **Primitive types** (`int`, `float`, `bool`, `char`) live on the stack. When the scope ends they are gone automatically — no manual release needed.
- **Heap-allocated types** (`string`, `[*, T]`, **closures**) are cleaned up when the variable goes out of scope. To free a dynamic array buffer **early** while the scope is still live, call `.clear()` from the standard library — the variable remains valid as an empty collection, never nil.

A plain named function declared inside a scope is just a function pointer — no heap allocation. A **closure** (an anonymous function that captures variables from the enclosing scope) allocates its captured environment on the heap. That environment is released when the closure variable goes out of scope:

```luc
let factor int = 3

-- plain function — just a pointer, no heap allocation
let double (x int) -> int = { return x * 2 }

-- closure — captures 'factor', heap-allocates the environment
let triple (x int) -> int = { return x * factor }
-- triple's captured environment is released when triple goes out of scope

let bigData byte[*] = loadFile("huge.bin")
-- ... use bigData ...
bigData.clear()    -- free heap buffer early; bigData is now empty but valid
```

Use nullable types (`int?`, `Vec2?`) when a value genuinely may be absent. Non-nullable types express the guarantee that a value is always present.

### Multiple Assignment

A function returning multiple values can be assigned to multiple variables in a single statement. Each variable requires its own explicit type annotation.

```
multi_assign      := decl_keyword var_spec { ',' var_spec } '=' expr

var_spec          := IDENTIFIER type_ann

decl_keyword      := 'let' | 'const'

-- Reassignment to existing variables (no let/const)
multi_assign_stmt := expr_lhs { ',' expr_lhs } '=' expr

expr_lhs          := IDENTIFIER
                   | expr '.' IDENTIFIER
                   | expr '[' expr ']'
                   -- any expression that can be an lvalue (assignable)
```

> [!NOTE]
> Reassignment rules
> - Each `expr_lhs` must be a valid lvalue: a variable name, a field access, or an array/slice index. Function calls, literals, and other rvalue expressions are not allowed.
> - The right-hand side must evaluate to exactly as many return values as there are left-hand side expressions.
> - The values are assigned from left to right.
> - This statement is only allowed in block scopes — not at top level.
> - Assigning to a `const` variable is a semantic error.

#### Declaration Form

Every variable has its own explicit type annotation, and the keyword (`let` or `const`) applies to all:

```luc
-- two return values
let value int, msg string = doSomething()

-- shorthand without per-variable keyword is NOT supported
let value, msg int = ...          -- ERROR: each variable needs its own type
let value int, msg string = ...   -- OK: implicit let on second variable

let q int, r int = divmod(10, 3)
const w int, h int = getScreenSize()
```

#### Reassignment Form

After variables are declared, reassign them using a multi-assignment statement:

```luc
let q int, r int    -- first declare
q, r = divmod(20, 6)  -- reassign

let x float, y float
x, y = 3.14, 2.718  -- ERROR: RHS must be a single expression

/--
 - A function that returns multiple values can be assigned
 - to multiple variables either at declaration time or later
 - as a reassignment.
--/
let a int, b string = f()   -- declaration
a, b = g()                  -- reassignment (multi_assign_stmt)
x, y = z, w                 -- ERROR: RHS must be single expression (a function call)
```

> [!WARNING]
> No type inference in multi-assignment — every variable must have an explicit type annotation. The right-hand side must be a single expression (i.e. a function call) that returns as many values as there are left-hand side targets. Comma-separated literal expressions are not allowed.

---

## Qualifiers

Qualifiers are part of the function type for `~async` and `~nullable`. Two functions with identical parameter and return types but different `~async` or `~nullable` qualifiers are **different types**. The `~parallel` qualifier does not affect type identity — it is an implementation attribute.

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

**Some qualifiers are part of type identity:**

```luc
-- Different types due to qualifiers
let f ~async    (a int) -> int = { ... }  -- different type
let g ~nullable (a int) -> int = nil      -- different type
let h ~parallel (a int) -> int = { ... }  -- same type as (a int) -> int

type Op = (a int) -> int

let op Op = f    -- ERROR: ~async vs no qualifier
let op Op = g    -- ERROR: ~nullable vs no qualifier
let op Op = h    -- OK: ~parallel doesn't affect type identity
```

```luc
let f ~async (a int) -> int = { ... }
let g (a int) -> int = { return a * 2 }

f = g    -- valid: same signature; f still carries ~async after reassignment
```

**Qualifier order is ignored for type equality.** Canonical display order is alphabetical:

```luc
-- identical
let f ~async ~nullable (a int) -> int = nil
let f ~nullable ~async (a int) -> int = nil
```

**Generics belong to the identifier, before qualifiers:**

```luc
let parallelFor<T> ~parallel (items T[*], body (item T)) = { ... }
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
> - **Qualifiers are NOT valid on anonymous function values.** An anonymous function is a plain value. Its qualifier context is determined by the declaration or parameter type it is assigned to, not by the value itself. See the Anonymous Functions section for details.
> - **Qualifiers on parameters are hints to the caller, not type gates.** The qualifier documents intent — it does not prevent a non-qualified function from being passed:

```luc
-- ~async on the parameter hints that task will be awaited inside run
let run (task ~async () -> int) -> int = {
    return await task()
}

-- both are valid — same underlying signature
run((url string) -> int { return 42 })                -- no qualifier on value
run((url string) -> int { return await fetch(url) })  -- ~async on declaration
```

### Call-Site Behavior

**`~async` — requires `await`:**

```luc
let fetch ~async (url string) -> string = { ... }

fetch("url")           -- ERROR: ~async binding must be called with await
await fetch("url")     -- OK

let process ~async (url string) -> string = {
    let raw string = await fetch(url)    -- OK: fetch is ~async, process is ~async
    return raw
}
```

> [!NOTE]
> When the compiler encounters `await expr`, it:
> 1. Evaluates `expr` — must resolve to a function call
> 2. Looks up the function being called
> 3. Checks whether that function's declaration carries the `~async` qualifier
> 4. If yes: valid, suspend and wait for result
> 5. If no: ERROR — cannot await a non-async function

**`~nullable` — requires nil guard:**

A variable of type `~nullable (A) -> B` cannot be assigned to a variable of type `(A) -> B` without a nil check. The call site must guard against nil.

```luc
let handler ~nullable (e Event) = nil

let plain (e Event) = handler   -- ERROR: cannot assign nullable to non-nullable
if handler != nil {
    let plain = handler          -- OK: inside guard, type is narrowed
}
```

**`~parallel` — body restrictions:**

When a function is called through a `~parallel`-qualified binding, the compiler enforces these restrictions inside the body function argument:

- No `return` statements
- No `break` or `continue`
- No `await` expressions
- No writes to variables declared outside the body scope

```luc
let parallelFor<T> ~parallel (items T[*], body (item T)) = { ... }

parallelFor(mesh.vertices, (vertex Vertex) {
    vertex.pos = vertex.pos |> transform    -- OK: local to this iteration
    result = 5                              -- ERROR: writes to outer scope
    return                                  -- ERROR: return in parallel body
    await fetch()                           -- ERROR: await in parallel body
})
```

### Attributes vs. Qualifiers

Both use a prefix symbol (`@` and `~`) and appear before a declaration or type, but they serve different purposes.

| Feature                  | Attribute (`@name`)                            | Qualifier (`~name`)                              |
| ------------------------ | ---------------------------------------------- | ------------------------------------------------ |
| **Attached to**          | Declaration (function, variable, struct, etc.) | Function **type** (or function binding)          |
| **Affects type?**        | No — metadata only                             | Yes for `~async`/`~nullable`; No for `~parallel` |
| **When evaluated**       | Compile time (by the compiler)                 | Type checking & call-site rules                  |
| **Examples**             | `@extern`, `@inline`, `@deprecated`            | `~async`, `~nullable`, `~parallel`               |
| **Can be user-defined?** | No (fixed set)                                 | No (fixed set)                                   |

**Why not make `~nullable` an attribute?**
An attribute would only mark the declaration. But then every call to a nullable function would need a nil check — that's a type-level contract, not mere metadata. A qualifier becomes part of the function's **type**, so the type system can enforce nil-guards and prevent assignment of a nullable function to a non-nullable variable.

**Why not make `@extern` a qualifier?**
`@extern` specifies linkage and ABI (calling convention, symbol name) — it does not affect the function's signature or how it is called in Luc code. A qualifier would incorrectly imply that an `@extern` function has a different type from a normal Luc function, which is not true.

### Type Aliases with Qualifiers

Since qualifiers are valid on function types, type aliases can carry them:

```luc
type AsyncOp      = ~async    (a int) -> int
type NullableOp   = ~nullable (a int) -> int
type ParallelBody = ~parallel (item int)
type AsyncMaybeOp = ~async ~nullable (a int) -> int
```

### Composition `+>` and Qualifiers

`+>` composes two functions only if neither operand has `~async` or `~nullable` qualifiers (i.e., both are plain functions). The resulting function is plain (no qualifiers). If you need to compose async or nullable functions, explicitly handle them before composition.

```luc
-- result is plain regardless of component qualifiers
let pipeline (url string) -> string = fetchData +> processData

-- assign qualifier to the result binding if needed
let pipeline ~async (url string) -> string = fetchData +> processData

-- nullable components must be guarded before composing
if transform != nil {
    let h = transform +> render
}
```

---

## Function Signatures

A function signature has three parts: qualifiers, parameter groups, and the return boundary `->`.

> [!NOTE]
> **Type identity:** The qualifiers `~async` and `~nullable` are part of the function's type. Two function types that differ only in these qualifiers are different types. The `~parallel` qualifier does not affect type identity.

```
func_decl       := [ visibility_mod ] decl_keyword IDENTIFIER [ generic_params ]
                   [ qualifier_list ] param_group { param_group }
                   [ '->' return_list ]
                   '=' func_body

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

`->` separates the parameter groups from the return types. Everything before `->` is input, everything after `->` is output.

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

**Multiple return** — comma separated and wrapped in `(...)` after `->`:

```luc
let f (a int) -> (int, string)
let g (src string) -> (int, bool, string)
```

> [!TIP]
> Write each return type on its own line when one of the return types is itself a function:
>
> ```luc
> -- Bad: return type too complex
> let parse (src string) -> (int, string, (a float) -> int)
>
> -- Good: each return type on its own line
> let parse (src string) -> (
>     int,
>     string,
>     (a float) -> int
> ) = {
>     ...
> }
> ```

### Currying

Multiple parameter groups express curry. Each `()` group is one call step. The compiler desugars this into a function that returns another function.

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
clamp0to100(42)                    -- 42
clamp0to100(150)                   -- 100

-- chains naturally with |> and +>
let addTen = add(10)
42 |> addTen |> string             -- "52"

let process = add(10) +> string    -- (b int) -> string
process(5)                         -- "15"
```

The compiler desugars multiple parameter groups into a function that explicitly returns an anonymous function. You always write the sugar form:

```luc
-- what you write
let add (a int)(b int) -> int = { return a + b }

-- what the compiler produces internally (never write this yourself)
let add (a int) -> (b int) -> int = { return (b int) -> int { return a + b } }
```

To close over a local variable (not a parameter), write the anonymous function explicitly:

```luc
let multiplier int = 3
let scale = (x int) -> int { return x * multiplier }
```

### Returned Curry Functions

A return type can itself be a curry function type. The `,` separates top-level return items. Each `->` belongs to its nearest preceding parameter group chain.

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
> Use type aliases for complex return types:
>
> ```luc
> type FloatParser = (s string)(n float) -> int
>
> let f (a int) -> (int, FloatParser) = {
>     ...
> }
> ```

### Anonymous Functions

An anonymous function is a **plain value** — it has no qualifiers. The qualifier context comes from the declaration or parameter type the anonymous function is assigned to, not from the function value itself.

> [!NOTE]
> The type of an anonymous function is inferred from context, and that type may include `~async` or `~nullable` if the context demands it (e.g., assigned to a `~async` variable or passed to a `~async` parameter).

```
anon_func   := param_group { param_group } [ '->' return_list ] block
               -- NO qualifier_list
               -- qualifiers belong to declarations, parameter types, and aliases
```

```luc
-- simple anonymous function
let Double (x int) -> int = (x int) -> int { return x * 2 }

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
let run (task ~async () -> int) -> int = {
    return await task()
}

run(() -> int { return 42 })            -- valid: signature matches
run(() -> int { return await fetch() }) -- also valid: body uses await correctly
```

> [!IMPORTANT]
> **Why no qualifiers on anonymous functions?**
> A qualifier describes how the compiler treats a call site. When a function is called, the compiler looks up the **binding's declaration** (or the **parameter type**) to find qualifiers — it never inspects the value itself. Therefore a qualifier on an anonymous function value is unreachable and meaningless.

### Generic Functions

Generic functions allow code to operate on multiple types using type parameters.

```
generic_params  := '<' generic_param { [','] generic_param } '>'

generic_param   := IDENTIFIER
                 | IDENTIFIER ':' IDENTIFIER
                 | IDENTIFIER ':' constraint_list

constraint_list := IDENTIFIER { '+' IDENTIFIER }
```

Generic parameters come immediately after the function name, before any parameter groups or qualifiers.

```luc
let identity<T> (v T) -> T = { return v }

let map<T, U> (items T[*], f (item T) -> U) -> U[*] = { ... }
```

**Type constraints** — constrain type parameters to traits using `:`. Multiple traits are joined with `+`:

```luc
let printSorted<T : Comparable + Printable> (items T[]) = { ... }
```

**Instantiation** — generic functions can be called with explicit type arguments:

```luc
let x int = identity<int>(42)
```

> [!NOTE]
> No type inference for generic functions — type must always be specified.

**Qualifiers** follow the generic parameter list:

```luc
let fetch<T> ~async (url string) -> T = { ... }
```

### Complete Signature Reference

```luc
-- void
let log (msg string) = { ... }

-- single return
let add (a int, b int) -> int = { ... }

-- multiple return
let f (a int) -> (int, string) = { ... }

-- multiple return, formatted
let parse (src string) -> (
    int,
    (a float) -> int
) = { ... }

-- curry
let add (a int)(b int) -> int = { ... }

-- deep curry
let clamp (min int)(max int)(value int) -> int = { ... }

-- curry with multiple return
let f (a int)(b string) -> (int, string) = { ... }

-- qualifiers
let fetch ~async    (url string) -> string = { ... }
let find  ~nullable (items int[*], pred (item int) -> bool) -> int = { ... }

-- generic (generic before qualifiers, always)
let map<T, U> (items T[*], f (item T) -> U) -> U[*] = { ... }

-- returned curry function, formatted
let f (a int) -> (
    int,
    (s string)(n float) -> int
) = { ... }

-- nullable return value
let f (a int) -> int? = { ... }

-- nullable function binding
let f ~nullable (a int) -> int = nil

-- nullable returned function via alias
type Transform = (v Vec2) -> Vec2
let f (a int) -> Transform? = { ... }

-- curry returning nullable function inline
let f (a int) -> ~nullable (b int) -> int = { ... }
```

---

## Struct Declaration

Structs can be declared at top level or inside a block. Local declarations **must not** have `pub` or `export`.

```
struct_decl     := [ visibility_mod ] 'struct' IDENTIFIER [ generic_params ]
                   '{' { field_decl } '}'

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
    objects T[]
}

struct Cache<K : Hashable + Comparable, V> {
    keys   []K
    values []V
}

-- struct literals
let origin Vec2  = Vec2  { x = 0.0  y = 0.0 }
let white  Color = Color {}                     -- all fields take defaults
let p Vec2<int>  = Vec2<int> { x = 1  y = 2 }

-- field access
let x float = origin.x    -- read
origin.x = 5.0            -- write (only valid if origin is 'let')
```

---

## Member Access

```
member_access   := expr '.' IDENTIFIER        -- data field
                 | expr ':' IDENTIFIER        -- method call (impl)
```

- `.` — data field access. Reassignable if the containing variable is `let`.
- `:` — impl method access. Never reassignable. Methods are plain function references and can be passed as values, stored in typed variables, or used as pipeline steps.

```luc
v.x                 -- read field
v.x = 5.0           -- write field (let only)
v:normalize         -- function reference to normalize from Vec2's impl
v:length            -- function reference to length from Vec2's impl
v:length = ..       -- ERROR: impl methods cannot be reassigned
```

---

## Enum Declaration

Enums can be declared at top level or inside a block. Local declarations **must not** have `pub` or `export`.

```
enum_decl       := [ visibility_mod ] 'enum' IDENTIFIER
                   '{' enum_variant { [','] enum_variant } '}'

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

## Type Aliases

Type aliases can be declared at top level or inside a block. Local declarations **must not** have `pub` or `export`.

```
type_decl       := [ visibility_mod ] 'type' IDENTIFIER [ generic_params ]
                   '=' type_alias_rhs

type_alias_rhs  := type    -- any valid type expression

generic_params  := '<' generic_param { [','] generic_param } '>'

generic_param   := IDENTIFIER
                 | IDENTIFIER ':' IDENTIFIER
                 | IDENTIFIER ':' constraint_list

constraint_list := IDENTIFIER { '+' IDENTIFIER }
```

A `type` alias introduces a new name for an existing shape — it does not create a new distinct type. For a distinct nominal type, use `struct`.

### Generic Parameters

Every generic parameter declared on a type alias **must appear at least once in the right-hand side**. An unused type parameter is a compile error.

```luc
type Pair<T>  = struct { first T, second T }   -- OK: T is used
type Wrap<T>  = T[]                             -- OK: T is used
type Gen<T>   = int                             -- ERROR: T is unused
type Odd<T>   = string                          -- ERROR: T is unused
```

If you intentionally want a type parameter that does not appear in the body (a phantom type), annotate with `@phantom`:

```luc
@phantom
type Tagged<T> = int   -- explicit opt-in: int tagged with T for type safety
```

This allows type-safe wrappers backed by the same primitive without accidental unused parameters slipping through silently.

### Qualifiers

Since qualifiers are part of the function type grammar, type aliases can carry them:

```luc
type AsyncOp      = ~async    (a int) -> int
type NullableOp   = ~nullable (a int) -> int
type ParallelBody = ~parallel (item int)
type AsyncMaybeOp = ~async ~nullable (a int) -> int
```

### Naming Conventions

Type alias names should reflect their kind at a glance. The following conventions are recommended:

| Kind                   | Convention              | Example                        |
| ---------------------- | ----------------------- | ------------------------------ |
| Slice (`T[]`)          | `...List` suffix        | `IntList`, `UserList`          |
| Dynamic array (`T[*]`) | `...Array` suffix       | `IntArray`, `UserArray`        |
| Fixed array (`T[*N]`)  | `...Buffer` suffix      | `ByteBuffer`, `FloatBuffer`    |
| Nullable (`T?`)        | `Maybe...` prefix       | `MaybeInt`, `MaybeUser`        |
| Result (`T!E`)         | `...Or...` — both sides | `IntOrString`, `UserOrDbError` |
| Function type          | `...Fn` suffix          | `ParserFn`, `HandlerFn`        |
| Nullable function      | `Maybe...Fn`            | `MaybeParserFn`                |
| Async function         | `...AsyncFn` suffix     | `FetchAsyncFn`                 |
| Parallel function      | `...ParallelFn` suffix  | `TransformParallelFn`          |

The `Or` convention for result types surfaces both the success and error type explicitly — `IntOrString` immediately tells you success is `int` and failure is `string` without opening the definition. Generic aliases need no convention — `<>` already signals the kind clearly.

> [!TIP]
> These are conventions only — the compiler does not enforce them. Combinations chain naturally: `MaybeUserOrDbError` (nullable success, can fail), `UserListOrDbError` (array that can fail).

### Examples

```luc
-- primitive alias
type ID = int

-- nullable alias
type MaybeInt  = int?
type MaybeUser = User?

-- result aliases — both sides named
type IntOrString   = int!string
type UserOrDbError = User!DbError

-- array aliases (postfix syntax)
type UserArray  = User[*]
type UserList   = User[]
type ByteBuffer = byte[*256]

-- array result — alias the array first, then apply Or convention
type UserArrayOrDbError = UserArray!DbError

-- function aliases
type ParserFn            = (src string) -> int
type FetchAsyncFn        = ~async    (url string) -> string
type TransformParallelFn = ~parallel (item Vec2)  -> Vec2

-- function result — alias the function first, then apply Or convention
type FetchAsyncFnOrNetError = FetchAsyncFn!NetworkError

-- nullable function alias
type MaybeParserFn = ~nullable (src string) -> int

-- array of function type — alias the function first, then array
type HandlerFn     = (event Event) -> bool
type HandlerFnList = HandlerFn[]       -- slice of handlers
type HandlerFnArray = HandlerFn[*]     -- dynamic array of handlers

-- generic alias — no convention needed
type Transform<T> = (v T) -> T
type Pair<K, V>   = struct { first K  second V }

-- constrained generic alias
type SortedPair<T : Comparable> = struct { first T  second T }
```

### Consistency Rule

Any type can have `impl` and `from` blocks — including primitives, structs, enums, and type aliases.

---

## Trait

```
trait_decl      := [ visibility_mod ] 'trait' IDENTIFIER [ generic_params ]
                   '{' { trait_method } '}'

trait_method    := IDENTIFIER [ qualifier_list ] param_group { param_group }
                   [ '->' return_list ]
                   -- signature only — no '=' and no body
```

**Rules:**

- Traits can be top-level or local. When local, `visibility_mod` must be omitted.
- No field declarations — method signatures only.
- No default implementations.
- A struct satisfies a trait by declaring `impl StructName : TraitName { ... }`.
- The impl declaration must provide every method in the trait.
- No function overloading — each method name must be unique.

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

## Impl Declaration

`impl` can be declared at top level or inside a block. Local declarations **must not** have `pub` or `export`.

```
impl_decl       := [ visibility_mod ] 'impl' impl_target
                   [ impl_generic_params ]
                   [ 'as' IDENTIFIER ]
                   [ ':' trait_ref ]
                   '{' { method_decl } '}'

impl_target     := type_name
                 | primitive_type

impl_generic_params := '<' impl_generic_param { ',' impl_generic_param } '>'

impl_generic_param  := IDENTIFIER [ ':' constraint_list ]

trait_ref       := IDENTIFIER [ generic_args ]

method_decl     := IDENTIFIER [ qualifier_list ] param_group { param_group }
                   [ '->' return_list ] '=' func_body
```

### Impl Target Rules

| Target type                                              | Allowed directly? | Example                                               |
| -------------------------------------------------------- | ----------------- | ----------------------------------------------------- |
| **Primitive** (`int`, `float`, `string`, `bool`, `char`) | ✅ Yes             | `impl int { isEven () -> bool = { … } }`              |
| **Struct**                                               | ✅ Yes             | `impl Vec2 { length () -> float = { … } }`            |
| **Enum**                                                 | ✅ Yes             | `impl Direction { isNorth () -> bool = { … } }`       |
| **Type alias**                                           | ✅ Yes             | `impl IntList { sum () -> int = { … } }`              |
| **Array type** (`[_, T]`, `[*, T]`, `[N, T]`)            | ✅ Yes             | `impl [_, int] { sum () -> int = { … } }`             |
| **Function type**                                        | ✅ Yes             | `impl (int) -> bool { negate () -> (int) -> bool … }` |
| **Trait**                                                | ❌ No              | Traits are contracts, not implementations             |

> [!WARNING] `impl` on array and function types
>
> **Array types** — methods defined on `[_, int]` apply to every slice of `int` in the visible scope. Use with care or prefer a named alias for clarity and narrower scope. There are no built-in array methods to shadow — the compiler only knows indexing and slicing.
>
> **Function types** — methods defined on a function type apply to every value of that exact signature in scope. Qualifiers are part of type identity: `impl (int) -> bool` does not cover `~async (int) -> bool`.
>
> **Array of function types** — both rules apply together. The element function type must be an exact signature match including qualifiers:
>
> ```luc
> impl [_, (int) -> bool] {
>     applyAll (value int) -> [_, bool] = {
>         let results [*, bool] = []
>         for f (int) -> bool in self { results:push(f(value)) }
>         return results
>     }
> }
> ```

### Generic Parameters on Impl Declarations

An `impl` block may declare generic parameters **only when the target type is generic** (a generic struct or a generic type alias). The number of generic parameters **must match** the arity of the target type. The parameter names are independent; they bind to the target's type parameters positionally.

| Type                     | Can we impl directly? | Generic impl allowed?   | Notes                                                     |
| ------------------------ | --------------------- | ----------------------- | --------------------------------------------------------- |
| Primitive (`int`, …)     | ✅ Yes                 | ❌ No                    | No generics on primitives                                 |
| Enum                     | ✅ Yes                 | ❌ No                    | Enums are not generic in Luc                              |
| Struct (non-generic)     | ✅ Yes                 | ❌ No                    |                                                           |
| Struct (generic)         | ✅ Yes                 | ✅ Yes, arity must match | `struct Box<T>` → `impl Box<T>`                           |
| Type alias (non-generic) | ✅ Yes                 | ❌ No                    |                                                           |
| Type alias (generic)     | ✅ Yes                 | ✅ Yes, arity must match | `type Pair<K,V>` → `impl Pair<K,V>`                       |
| Array type               | ✅ Yes                 | ❌ No                    | Methods apply to all arrays of that element type in scope |
| Function type            | ✅ Yes                 | ❌ No                    | Qualifier is part of identity — exact match only          |
| Trait                    | ❌ No                  | N/A                     | Traits are contracts, not implementations                 |

When the target type is generic but the impl declaration omits generic parameters, the compiler requires them in method signatures — you must declare the type parameters to use them.

```luc
-- Invalid: T is unknown without generic parameters
impl Box {
    get () -> T = { return self.value }   -- ERROR: T unknown
}

-- Valid: declare generic parameters to use them in method signatures
impl Box<T> {
    get () -> T = { return self.value }   -- OK
}
```

### Examples

```luc
-- No alias, no generics, no trait
impl Vec2 {
    length () -> float = { return #sqrt(self.x*self.x + self.y*self.y) }
}

-- With generics, no alias
impl Box<T> {
    get () -> T = { return self.value }
}

-- With generics and alias
impl Box<T> as b {
    get () -> T = { return b.value }
}

-- With alias and trait conformance
impl Circle as c : Drawable {
    draw () { c:render() }
}

-- Primitive with alias
impl int as i {
    isEven () -> bool = { return i % 2 == 0 }
}

-- Generic struct
struct Box<T> { value T }

impl Box<T> {
    get () -> T = { return self.value }
}

-- Generic type alias
type Result<T, E> = struct { ok T?, err E? }

impl Result<T, E> {
    isOk () -> bool = { return self.ok != nil }
}

-- ERROR cases
impl int<T> { ... }      -- ERROR: primitive cannot have generics
impl Enum<T> { ... }     -- ERROR: enum cannot have generics
impl Vec2<T> { ... }     -- ERROR: non-generic struct cannot have generics
impl Pair<X> { ... }     -- ERROR: arity mismatch (needs 2 parameters, got 1)
```

---

## From Declaration

`from` can be declared at top level or inside a block. Local declarations **must not** have `pub` or `export`.

```
from_decl   := [ visibility_mod ] 'from' type [ generic_params ] '{' { from_entry } '}'

from_entry  := param_group { param_group } '->' type '=' func_body
               -- source param(s), target type name, body
               -- target type name must match the enclosing from target
```

A `from` block defines implicit and explicit conversions from a source type (described by the parameter groups) to a target type. The target type can be **any type** — primitive, struct, enum, type alias, array type, or function type.

### Target Types

| Target Type       | Example                                                       | Notes                            |
| ----------------- | ------------------------------------------------------------- | -------------------------------- |
| **Primitive**     | `from int { (x string) -> int = { ... } }`                    | Convert from string to int       |
| **Struct**        | `from Fahrenheit { (c Celsius) -> Fahrenheit = { ... } }`     | Convert between structs          |
| **Enum**          | `from Direction { (s string) -> Direction = { ... } }`        | Parse string to enum variant     |
| **Type alias**    | `from ID { (x int) -> ID = { ... } }`                         | Convert to aliased type          |
| **Array type**    | `from [_, int] { (r Range) -> [_, int] = { ... } }`           | Convert to a slice of int        |
| **Function type** | `from (int) -> bool { (fn ~nullable ...) -> (int) -> bool… }` | Convert to a plain function type |

### Rules

Each `from` entry contains:

- One or more parameter groups (currying allowed) — the source value(s).
- The arrow `->` (mandatory).
- A single return type — must be the type named in the enclosing `from` declaration.
- `=` followed by the conversion body (a block or expression).
- No qualifiers (`~async`, `~nullable`, `~parallel`) — `from` entries are plain functions.

The compiler does not chain conversions (e.g., A → B → C) — only a single direct conversion is applied.

> [!NOTE] `from` on array and function types
>
> **Array types** — the `from` target is the specific array kind and element type. `from [_, int]` only applies when the target type is exactly `[_, int]` (a slice of int). A `from [*, int]` is a separate declaration for dynamic arrays of int.
>
> **Function types** — qualifiers are part of type identity. `from (int) -> bool` does not cover `from ~async (int) -> bool`. This is useful for stripping or adding qualifiers during conversion:
>
> ```luc
> -- convert a nullable function to a plain one with a fallback
> from (int) -> bool {
>     (f ~nullable (int) -> bool) -> (int) -> bool = {
>         if f == nil { return (x int) -> bool { return false } }
>         return f
>     }
> }
> ```

### Scope and Visibility

`from` declarations can appear in any scope (top-level or local) and are visible following standard lexical scoping rules:

- A `from` declaration is visible in the scope where it is declared and all nested scopes.
- Multiple `from` declarations for the same target type are allowed in different scopes.
- When looking for a conversion, the compiler searches from the innermost scope outward, using the first matching conversion found.
- If multiple visible `from` declarations provide a conversion from the same source to the same target with the same signature, the nearest (innermost scope) wins; ambiguous conversions produce a compile error.

### Implicit Casting Contexts

When a `from` declaration exists for target `T` accepting source `S`, the compiler automatically desugars in these contexts:

1. **Variable declaration** — `let m Minutes = rawSecs`
2. **Function arguments** — `doubleKm(d)` where `d` is `Meters`
3. **Function return** — assigning `Celsius` return to `Fahrenheit` variable
4. **Assignment** — `currentTemp = newReading`

### Examples

```luc
-- Convert from string to int (primitive target)
from int {
    (s string) -> int = {
        return #parseInt(s)
    }
}

-- Convert from Celsius and Kelvin to Fahrenheit (struct target)
export from Fahrenheit {
    (c Celsius) -> Fahrenheit = {
        return Fahrenheit { value = c.value * 9.0 / 5.0 + 32.0 }
    }
    (k Kelvin) -> Fahrenheit = {
        return Fahrenheit { value = (k.value - 273.15) * 9.0 / 5.0 + 32.0 }
    }
}

-- Convert from Celsius to Kelvin (struct target)
pub from Kelvin {
    (c Celsius) -> Kelvin = {
        return Kelvin { value = c.value + 273.15 }
    }
}

-- Convert from string to Direction (enum target)
from Direction {
    (s string) -> Direction = {
        match s {
            "north" => Direction.North
            "south" => Direction.South
            "east"  => Direction.East
            "west"  => Direction.West
            default => Direction.North
        }
    }
}

-- Generic from declaration
struct Wrapper<T> { value T }

from Wrapper<T> {
    (val T) -> Wrapper<T> = {
        return Wrapper<T> { value = val }
    }
}

-- Local from declaration (only visible inside this function)
let process () -> int = {
    from string {
        (s string) -> int = {
            return #parseInt(s)
        }
    }

    let x int = "42"  -- uses the local from declaration
    return x
}

-- From on array type directly (no alias required)
from [_, int] {
    (r Range) -> [_, int] = {
        let result int[*] = []
        for i int in r { result:push(i) }
        return result
    }
}

from [*, int] {
    (arr [_, int]) -> [*, int] = {
        let result int[*] = []
        for v int in arr { result:push(v) }
        return result
    }
}

-- From on function type directly (no alias required)
-- strips ~nullable qualifier by providing a plain fallback
from (int) -> bool {
    (f ~nullable (int) -> bool) -> (int) -> bool = {
        if f == nil { return (x int) -> bool { return false } }
        return f
    }
}

-- Call site examples
let boiling Celsius      = Celsius { value = 100.0 }
let hot     Fahrenheit   = Fahrenheit(boiling)           -- uses from declaration
let temp    int          = int("123")                    -- uses from int block
let dir     Direction    = Direction("north")            -- uses from Direction block
let wrapped Wrapper<int> = Wrapper<int>(42)              -- uses generic from declaration
let nums    [_, int]     = [_, int](Range { lo=0 hi=5 }) -- uses from [_, int] block
```

---

## Pipeline Operator `|>`

`|>` executes a chain left-to-right at runtime.

```
pipeline_expr   := pipeline_seed { '|>' pipeline_step }

pipeline_seed   := expr

pipeline_step   := IDENTIFIER
                 | expr ':' IDENTIFIER                 -- method call on value
                 | IDENTIFIER '.' IDENTIFIER           -- non-nullable data field
                 | IDENTIFIER '(' arg_list ')' '!'     -- argument pack
                 | anon_func
```

| Step form      | What `\|>` does                 | Nullability                |
| -------------- | ------------------------------- | -------------------------- |
| `fn`           | calls `fn(upstream)`            | must be non-nullable       |
| `var:method`   | calls `method(upstream)`        | always safe                |
| `struct.field` | calls function stored in field  | field must be non-nullable |
| `fn(args)!`    | calls `fn(upstream, args...)`   | must be non-nullable       |
| `anon_func`    | calls with upstream as argument | always safe                |

> [!NOTE]
> `var:method` — `var` is the variable the method is dispatched on, not a type name. `:` is the method call operator in Luc (see Member Access). The upstream value is passed as the argument to the method, not as the receiver. For example, `v |> list:push` passes `v` as the argument to `push` on `list`.

### The `!` Argument Pack Annotation

> [!NOTE]
> The `!` is **forbidden** for anonymous functions.

`fn(args)!` is not a function call — the `!` marks an intentionally incomplete argument list. The upstream value is injected as the first argument when `|>` fires.

```luc
let scale (v Vec2, factor float) -> Vec2 = { ... }

v |> scale(2.0)!     -- calls scale(v, 2.0)
v |> scale(2.0)      -- ERROR: looks like a complete call, no place for upstream
```

### Rules

**Qualifier behavior in pipeline steps:**

- Steps with `~async` are allowed. The pipeline expression becomes `~async` (its type includes `~async`). The caller must `await` the entire pipeline result.
- Steps with `~nullable` are forbidden.
- Steps with `~parallel` are forbidden — pipeline execution is synchronous.

> [!WARNING] Alias bypass is not allowed
> The qualifier check is enforced on the **resolved underlying type**, not the surface syntax. A variable whose type alias wraps a `~nullable` or `~parallel` function does not bypass the restriction — the compiler looks through the alias chain before accepting a step.
>
> ```luc
> type MaybeParserFn       = ~nullable (src string) -> int
> type TransformParallelFn = ~parallel (item Vec2)  -> Vec2
>
> -- f and g are concrete function variables
> let f MaybeParserFn       = nil
> let g TransformParallelFn = (item Vec2) -> Vec2 { return item }
>
> -- both ERROR: underlying qualifier is forbidden as a pipeline step
> "hello" |> f
> origin  |> g
>
> -- guard first, then call directly — never as a pipeline step
> if f != nil {
>     let result int = f("hello")    -- OK: guarded, called directly
> }
> ```

**Data fields as pipeline steps:**

A struct data field of function type can be a pipeline step only if its type is non-nullable:

```luc
struct Processor {
    transform  (v Vec2) -> Vec2          -- non-nullable: safe as step
    onComplete ~nullable ()              -- nullable: must guard first
}

let p Processor = Processor { transform = vec.normalize }

v |> p.transform               -- OK
v |> p.onComplete              -- ERROR: onComplete is ~nullable

if p.onComplete != nil {
    p.onComplete()             -- OK: guarded
}
```

### `|>` vs `+>` — Key Difference

|                 | `\|>` pipeline                   | `+>` composition          |
| --------------- | -------------------------------- | ------------------------- |
| When            | runtime                          | compile time              |
| Seed            | required                         | none                      |
| Control flow    | full access                      | none                      |
| Type strictness | relaxed — step may ignore result | strict — types must chain |
| Result          | executes now                     | produces a new function   |

---

## Composition Operator `+>`

`+>` wires functions together at compile time without executing them. The output type of the left must exactly match the input type of the right.

```
compose_expr    := pipeline_expr { '+>' compose_operand }

compose_operand := IDENTIFIER
                 | expr ':' IDENTIFIER          -- method reference on a value
                 | expr '.' IDENTIFIER          -- non-nullable data field only
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

Generic functions must be explicitly instantiated before composing — type inference across `+>` is not supported:

```luc
let doubleInt   (x int) -> int    = { ... }
let intToString (x int) -> string = { ... }

let process = doubleInt +> intToString    -- valid: both concrete
```

> [!WARNING]
> Alias bypass is not allowed
> The qualifier check for `+>` is enforced on the **resolved underlying type**. A variable whose type alias wraps a `~nullable` function is still forbidden as a composition operand — the compiler looks through the alias chain.
>
> ```luc
> type MaybeTransformFn = ~nullable (v Vec2) -> Vec2
>
> -- f is a concrete nullable function variable
> let f MaybeTransformFn = nil
>
> -- ERROR: f's underlying type is ~nullable, forbidden in +>
> let pipeline (v Vec2) -> Vec2 = normalize +> f
>
> -- guard first to narrow the type, then compose
> if f != nil {
>     let pipeline (v Vec2) -> Vec2 = normalize +> f    -- OK: guarded, known non-nullable
> }
> ```

---

## Async / Await

`~async` marks a function binding as asynchronous. `await` suspends the current function until the awaited call resolves.

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

- `await` is only valid inside a function body whose binding carries `~async`.
- `await expr` — `expr` must resolve to a call to a `~async`-qualified function.
- An `~async` function may freely call non-async functions.
- `await` is not valid inside a `~parallel` body function.

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
                 | '::' IDENTIFIER '.' IDENTIFIER
                 | '?.' IDENTIFIER
                 | '??' expr                  -- lhs is nil or T!E: result is rhs
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
                 | resolve_expr

resolve_expr    := 'resolve' expr '{' ok_arm err_arm '}'

ok_arm          := 'ok'  '(' IDENTIFIER type ')' block

err_arm         := 'err' '(' [ IDENTIFIER type ] ')' block

await_expr      := 'await' expr
                   -- expr must be a call to a ~async-qualified function
                   -- valid only inside a ~async function body

range_expr      := expr range_op expr [ '..' expr ]

range_op        := '..'     -- inclusive end
                 | '..<'    -- exclusive end

arg_list        := expr { [','] expr }
```

---

## Arrays

Three distinct kinds:

| Kind    | Syntax   | Memory           | Growable |
| ------- | -------- | ---------------- | -------- |
| Slice   | `[_, T]` | fat pointer view | ❌        |
| Dynamic | `[*, T]` | heap-owned       | ✅        |
| Fixed   | `[N, T]` | stack/inline     | ❌        |

Array syntax encloses both the kind marker and the element type inside brackets. The closing `]` unambiguously marks the end of the array type before any suffix is encountered — `?` and `!` always attach to the whole array, and inline function types are fully supported as element types without requiring an alias.

```luc
let rgba [4, float]  = [1.0, 0.5, 0.0, 1.0]
let nums [*, int]    = [10, 20, 30, 40, 50]
let view [_, int]    = nums[1..3]     -- elements at index 1, 2, 3 (inclusive)
let excl [_, int]    = nums[1..<3]    -- elements at index 1, 2 (exclusive end)
```

Suffix binding is completely unambiguous:

```luc
[_, int]!string     -- ([_, int])!string   — slice of int, op can fail with string
[*, int]?           -- ([*, int])?         — nullable dynamic array
[4, int]!DbError    -- ([4, int])!DbError  — fixed array, op can fail
[_, int?]           -- slice of nullable int
[_, int!string]     -- slice of result elements
```

Nested arrays:

```luc
[_, [*, int]]        -- slice of dynamic arrays of int
[*, [_, string]]     -- dynamic array of slices of string
[4, [_, float]]      -- fixed array of 4 slices of float
```

> [!NOTE]
> - **Array elements are not nullable by default.** To allow nil elements use `T?`: `let nums [*, int?] = [1, nil, 3]`
> - **Out of bounds:** Fixed and slice arrays panic at runtime. Dynamic arrays return nil for out-of-bounds index access.

### Arrays of Function Types

Inline function types are fully supported as element types — the closing `]` makes the boundary unambiguous. No alias is required, though aliases are still recommended for named reuse:

```luc
-- inline function type as element — fully unambiguous
let predicates  [_, (int) -> bool]              = [isEven, isPositive, isPrime]
let asyncTasks  [*, ~async (url string) -> string] = [fetchUser, fetchPosts]
let handlers    [4, (event Event) -> bool]      = [onKeyDown, onKeyUp, onClick, onScroll]

-- alias still useful for reuse and naming
type PredicateFn  = (int) -> bool
type FetchAsyncFn = ~async (url string) -> string

let predicates  [_, PredicateFn]   = [isEven, isPositive, isPrime]
let asyncTasks  [*, FetchAsyncFn]  = [fetchUser, fetchPosts]
```

**Allowed operations:**

| Operation            | Example                         | Notes                                                                      |
| -------------------- | ------------------------------- | -------------------------------------------------------------------------- |
| Store function       | `handlers[0] = validate`        | The function type must match the array's element type exactly.             |
| Call through index   | `let result = handlers[i](arg)` | The index must be an integer; the call follows normal rules.               |
| Pass as argument     | `applyAll(handlers, data)`      | The array is passed by value (owned) or by reference (`&[_, T]`).          |
| Return from function | `return getCallbacks()`         | Ownership follows array semantics (deep copy for dynamic, view for slice). |

> [!WARNING] Restrictions on arrays of function types
>
> 1. **No equality** — Function types are not comparable (`==`, `!=`). Arrays of functions are also not comparable.
>
> ```luc
> let a [_, (int) -> bool] = [f]
> let b [_, (int) -> bool] = [f]
> if a == b { ... }   -- ERROR: cannot compare arrays of function type
> ```
>
> 2. **Qualifiers affect element type** — `[_, ~async (int) -> int]` is distinct from `[_, (int) -> int]`. The qualifier is part of the element type's identity.
>
> ```luc
> let asyncFn ~async (x int) -> int = { ... }
> let arr [_, ~async (x int) -> int] = [asyncFn]   -- OK: types match
>
> let result = await arr[0](5)    -- OK: caller uses await
> let result = arr[0](5)          -- ERROR: ~async called without await
> ```
>
> 3. **Closure capture** — If a stored function captures variables (a closure), the array holds a reference to the closure's environment. Use the standard library's `.clear()` on dynamic arrays to release closures early.
>
> 4. **No generic specialisation** — The element type is a concrete function signature. Generic functions cannot be stored directly unless instantiated.
>
> ```luc
> let idInt   (int) -> int        = identity<int>
> let arr     [_, (int) -> int]   = [idInt]    -- OK: concrete instantiation
> let bad     [_, (T) -> T]       = [identity] -- ERROR: generic without type arguments
> ```

> [!TIP] Arrays of function types enable
> - **Dispatch tables** — Replace switch/match with indexed function lookup.
> - **Callback lists** — Event handlers, middleware chains, plugin systems.
> - **Higher-order collections** — Store partially applied functions, curried functions, or stateful closures.
> - **Interpreters & DSLs** — Represent operations as functions in a data structure.

### Slice Range Rules

| Operator | End bound | Example on `[10,20,30,40,50]`              |
| -------- | --------- | ------------------------------------------ |
| `..`     | inclusive | `nums[1..3]` → `[20, 30, 40]` (3 elements) |
| `..<`    | exclusive | `nums[1..<3]` → `[20, 30]` (2 elements)    |

### Compiler-Built Operations

The compiler has direct knowledge of three array operations only. Everything else is provided by the standard library or implemented by the user via `impl`.

**Indexing and slicing — built into the compiler:**

| Operation    | Returns  | Description      |
| ------------ | -------- | ---------------- |
| `arr[i]`     | `T`      | element at index |
| `arr[i..j]`  | `[_, T]` | inclusive slice  |
| `arr[i..<j]` | `[_, T]` | exclusive slice  |

> [!NOTE]
> - **Out of bounds:** Fixed and slice arrays panic at runtime. Dynamic arrays return nil for out-of-bounds index access.
> - **Concatenation** (`+`) on `[_, T]` and `[*, T]` is also built into the compiler — it produces a new array containing all elements of both operands.

### Standard Library Methods

All other array operations — `.len()`, `.push()`, `.pop()`, `.clear()`, `.reserve()`, etc. — are provided by the standard library as `impl` blocks on the array types. The standard library is included by default but is not part of the language specification.

Users who want different semantics can write their own `impl` blocks. The compiler does not enforce any particular method names or signatures beyond the three built-in operations above.

```luc
-- example: user-defined impl on a slice type
impl [_, int] {
    sum () -> int = {
        let total int = 0
        for v int in self { total += v }
        return total
    }
}
```

> [!NOTE]
> The standard library's `impl` blocks for array types follow the same shadowing rules as any other `impl` — a local `impl` in a narrower scope wins over the std library's `impl` in the outer scope. This means users can selectively override individual methods without replacing the entire standard implementation.

---

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
> **Visibility inside blocks:** `pub` and `export` are **not allowed** on any local declaration — they are top-level only. The parser emits an error if they appear inside a block.
>
> **Attributes on local declarations:** Attributes (`@inline`, `@deprecated`, `@unroll`, etc.) **are allowed** on local declarations (`type`, `struct`, `enum`, `impl`, `from`, `var`, `func`). They are attached to the declaration node and may be used by the semantic pass.

### Examples of Valid Local Declarations

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
```

### If / Else — Statement Form

```
if_stmt         := 'if' expr block [ 'else' ( if_stmt | block ) ]
```

`else` is optional. Type narrowing applies inside the then-branch when the condition is an `is` expression.

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

`else` is **required** in expression form. Both branches must produce compatible types. `??` is the separator between condition and then-branch — it is not the null-coalescing operator in this context.

```luc
let grade string = if score >= 60 ?? "pass" else "fail"

-- chained (right-associative)
let label string = if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive"
```

### Match

`match` is expression-oriented — it produces a value. `default` is required and must be last. Arm bodies are expressions only — no blocks, no `return`.

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
    200     => "ok"
    404     => "not found"
    default => "unknown"
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
    200     => "ok",      "request succeeded"
    404     => "not found"   -- detail is implicitly nil
    default => "unknown"
}
```

### Switch

`switch` is statement-oriented — dispatches on a value, runs blocks, produces no value.

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
for_stmt    := 'for' IDENTIFIER type_ann 'in' ( range_iter | expr ) [ '..' expr ] block

range_iter  := expr range_op expr
               -- start range_op end
               -- type_ann must be numeric (int, float, etc.) for range iteration
```

```luc
for i int in 0..10  { io.printl(string(i)) }          -- 0 through 10 inclusive
for i int in 0..<10 { io.printl(string(i)) }          -- 0 through 9
for i int in 0..10..2 { io.printl(string(i)) }        -- step of 2
for item string in items { process(item) }             -- collection iteration
```

> [!NOTE]
> - The iteration variable must have an explicit type annotation (no type inference).
> - For range iteration, the type must be numeric (`int`, `float`, `double`, etc.).
> - For collection iteration, the type must match the element type of the iterable.
> - The optional step expression (after the second `..`) is only valid for range iteration.
> - The step expression must be of the same numeric type as the iteration variable.

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

- Struct types — implement `Equatable<T>` and use `:equals()` instead.
- Function types — never comparable regardless of qualifiers. Even if two functions have identical types (including qualifiers), `==` is not allowed.
- Array types — use collection library comparison.

### `===` — Reference Equality

Checks same memory address. Valid for `&T`, structs, nullable reference types.

### `is` — Type Identity

Checks the type of a value. Narrows the type inside the enclosing block (statement form only). Nullable and non-nullable are distinct types.

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
if getUser() != nil and validate(getUser()) { ... }    -- short-circuit
if cache:has(key) or expensiveLoad(key) { ... }        -- short-circuit
```

`not` operates on `bool` and nullable types:

```luc
if not isValid { ... }
if not x { ... }    -- x is nullable: nil treated as false, not flips to true
```

---

## Bitwise Operators

Integer types only. `&&` and `||` are bitwise AND/OR (not logical — those use `and`/`or` keywords). This avoids ambiguity with `&` (reference operator).

| Operator | Name                |
| -------- | ------------------- |
| `&&`     | bitwise AND         |
| `\|\|`   | bitwise OR          |
| `~^`     | bitwise XOR         |
| `~~`     | bitwise NOT (unary) |
| `<<`     | left shift          |
| `>>`     | right shift         |

```luc
let flags   uint32 = 0xFF00
let mask    uint32 = 0x0F0F
let result  uint32 = flags && mask     -- 0x0F00
let merged  uint32 = flags || mask     -- 0xFF0F
let inv     uint32 = ~~flags           -- bitwise NOT
let shifted uint32 = 1 << 4           -- 16
```

---

## Error Handling

Luc treats errors as values. There are no exceptions, no stack unwinding, and no hidden control flow. Every function that can fail declares this in its return type using the `!` suffix, and every `T!E` value is **inert** until explicitly resolved — the compiler forbids using it as a plain `T`.

### Result Types

`!` is a suffix on the return type that separates the success type from the error type:

```
result_type     := type '!' type    -- succeeds with T, fails with E
                 | type '?' '!' type -- succeeds with T? (nullable), fails with E
                 | type '!'          -- succeeds with T, fails with nil
                 | type '?' '!'      -- succeeds with T? (nullable), fails with nil
```

The four combinations:

```luc
int!string      -- succeeds with int,  fails with string
int?!string     -- succeeds with int?, fails with string
int!            -- succeeds with int,  fails with nil
int?!           -- succeeds with int?, fails with nil
```

The error type is always explicit — no built-in `Error` type. The programmer owns what an error looks like entirely. Bare `!` with no error type means the failure carries nil — the caller knows it failed but receives no detail.

### `!` Binding Rules

`!` binds to the immediately preceding **named or primitive** type token only. With postfix array syntax, `!` after an array type attaches to the whole array unambiguously — the bracket closes the array type before `!` is encountered:

```luc
int[]!string      -- (int[])!string    — slice of int, op can fail with string
int[*]!DbError    -- (int[*])!DbError  — dynamic array, op can fail
int!string[]      -- (int!string)[]    — slice of result elements
int?[]!string     -- (int?[])!string   — slice of nullable int, op can fail
```

Inline function types still require an alias before `!` can attach to the whole function type — the function return type bleeds into what follows, making the binding ambiguous:

```luc
-- AMBIGUOUS: does ! attach to the function type as a whole, or to its return type int?
(src string) -> int!string

-- parser reads: (src string) -> (int!string) — function returning int!string
-- to make the whole function type the success value, alias first:
type ParserFn       = (src string) -> int
let f () -> ParserFn!string = { ... }    -- (ParserFn)!string — unambiguous
```

Valid without alias — primitives, structs, enums, array types, and named aliases:

```luc
int!string          -- OK: primitive
Vec2!string         -- OK: struct
Direction!string    -- OK: enum
int[]!string        -- OK: postfix array — unambiguous
int[*]!DbError      -- OK: postfix dynamic array — unambiguous
UserArray!DbError   -- OK: named alias
```

### `?` on a Result Type

`?` after `!E` binds to `E`, not to the whole `T!E` — the same binding rule applied consistently:

```luc
int!string?     -- int!(string?)  — error type is nullable string
                -- ? binds to string, not to the whole int!string
```

To make the whole result type nullable, alias first:

```luc
type IntOrString   = int!string
let x IntOrString? = nil    -- nullable result — the whole T!E may be absent
```

### Nesting `!` is Forbidden

Neither the success type nor the error type in `T!E` may itself carry `!`. The compiler rejects any nested result type.

```luc
int!string           -- OK
int!(string!int)     -- ERROR: error type cannot itself be a result type
(int!string)!int     -- ERROR: success type cannot itself be a result type
```

Both readings are nonsensical. A success type carrying `!` means you receive an inert unresolved value on success — defeating the purpose of resolving the outer `!`. An error type carrying `!` means the error itself can fail, which has no meaningful interpretation.

If a function can fail in multiple distinct ways, model the failure explicitly with a struct or enum rather than nesting result types:

```luc
enum FetchError { Network  Parse  Timeout }

struct FetchFailure {
    kind    FetchError
    message string
}

-- single ! with a rich error type — clear, exhaustive, no nesting needed
let fetch (url string) -> string!FetchFailure = { ... }
```

### Forbidden Operations on `T!E`

A `T!E` value is completely inert until resolved. The following are all compile errors:

```luc
let x int!string = riskyOp()

x + 1                   -- ERROR: cannot apply '+' to unresolved int!string
x * 2                   -- ERROR: cannot apply '*' to unresolved int!string
let y = x               -- ERROR: cannot assign unresolved int!string
io.printl(x)            -- ERROR: cannot pass unresolved int!string as argument
string(x)               -- ERROR: cannot convert unresolved int!string
x.field                 -- ERROR: cannot access field on unresolved int!string
x:method()              -- ERROR: cannot call method on unresolved int!string
x |> double             -- ERROR: cannot pipe unresolved int!string
x == 5                  -- ERROR: cannot compare unresolved int!string
```

The only legal operations on a `T!E` value are the two resolution strategies below.

### Resolution Strategy 1 — `resolve` Block

`resolve` is a keyword that forces you to handle both outcomes before the value is usable. The `ok` arm receives the plain unwrapped `T`, the `err` arm receives the error value of type `E`. Both arms are required. Both must return the same type. After the block, the result is plain `T` — the `!` is consumed.

```
resolve_expr    := 'resolve' expr '{' ok_arm err_arm '}'

ok_arm          := 'ok'  '(' IDENTIFIER type ')' block
                   -- type is plain T, never T!E — ! is consumed here

err_arm         := 'err' '(' [ IDENTIFIER type ] ')' block
                   -- type matches the error type declared after !
                   -- parens are empty when error type is nil (bare !)
```

```luc
-- int!string: ok receives int, err receives string
let result int = resolve divide(10, 0) {
    ok  (v int)    { return v }
    err (e string) { return -1 }
}

-- int!: ok receives int, err receives nothing (bare ! = nil error)
let result int = resolve validate(id) {
    ok  (v int) { return v }
    err ()      { return 0 }    -- empty parens: no error value to receive
}

-- int?!string: ok receives int? (still nullable after resolve), ?? handles nil
let result int = resolve findUser(id) {
    ok  (v int?)   { return v }    -- v is int?, pass through
    err (e string) { return nil }
} ?? 0                             -- ?? handles the nil case after ! is consumed
```

Explicit propagation — if the current function also returns `T!E`, return the error directly from the `err` arm:

```luc
let process (url string) -> string!DbError = {
    let raw string = resolve fetchData(url) {
        ok  (v string)   { return v }
        err (e DbError)  { return e }    -- propagates: process returns DbError here
    }

    let parsed string = resolve parseJson(raw) {
        ok  (v string)   { return v }
        err (e DbError)  { return e }
    }

    return formatOutput(parsed)
}
```

### Resolution Strategy 2 — `??` Fallback

`??` is the shorthand resolution strategy. It triggers the right-hand side when the left-hand side is **either** `nil` **or** an unresolved `!`. After `??`, the result is always plain `T`.

```
fallback_expr   := expr '??' expr
                   -- LHS is plain T (non-nil, non-!): result is T, RHS never evaluated
                   -- LHS is nil: result is RHS
                   -- LHS is T!E (unresolved): error is discarded, result is RHS
```

```luc
-- triggers: lhs is nil
let a int? = nil
let b int  = a ?? 0

-- triggers: lhs is unresolved !
let c int!string = riskyOp()
let d int        = c ?? 0    -- error discarded, d = 0

-- never triggers: lhs is plain int
let e int = getValue()
let f int = e ?? 0    -- always e

-- ??  chains naturally after resolve when T is nullable
let result int = resolve findUser(id) {
    ok  (v int?)   { return v }
    err (e string) { return nil }
} ?? 0
```

> [!NOTE]
> Use `??` when you have a sensible default and do not need to inspect the error. Use `resolve` when you need to log, handle, or propagate the error detail.

### Error Handling in Pipelines

`T!E` cannot be a pipeline step directly — every step must be a function, and `resolve` is an expression, not a function. To resolve inside a pipeline, use an anonymous function as the final step:

```luc
-- anonymous function receives T!E, resolves it, returns plain T
type StringOrString = string!string    -- alias following Or convention

let result string = dbFindUser(id)
    |> formatUser!                              -- User -> StringOrString, can fail
    |> (v StringOrString) -> string {           -- anonymous function receives T!E
        return resolve v {
            ok  (s string) { return s }
            err (e string) { return "unnamed" }
        }
    }
```

`??` can also appear at the end of a pipeline directly when no error detail is needed:

```luc
let result string = fetchData(url)
    |> parseJson
    |> formatOutput
    ?? ""    -- any step that failed: discard error, use ""
```

### Complete Example

```luc
struct User {
    id    int
    name  string
    email string
}

struct DbError {
    code    int
    message string
}

-- result type aliases following the Or convention
type MaybeUserOrDbError  = User?!DbError
type StringOrString      = string!string

let dbFindUser (id int) -> MaybeUserOrDbError = {
    if id < 0 {
        return DbError { code = 2  message = "invalid id" }
    }
    return db:query(id)    -- returns User?, nil if not found
}

let formatUser (user User) -> StringOrString = {
    if user.name == "" { return "user has no name" }
    return user.name + " <" + user.email + ">"
}

let getFormattedUser (id int) -> string = {

    -- resolve with full error handling
    -- ok receives User? (still nullable), ?? handles nil after
    let user User = resolve dbFindUser(id) {
        ok  (v User?)   { return v }
        err (e DbError) {
            system.logError(e.code, e.message)
            return nil
        }
    } ?? User { id = 0  name = "guest"  email = "" }

    -- pipeline with anonymous function to resolve mid-chain
    let result string = user
        |> formatUser
        |> (v StringOrString) -> string {
            return resolve v {
                ok  (s string) { return s }
                err (e string) {
                    io.printl("format failed: " + e)
                    return user.name
                }
            }
        }

    return result
}
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
| 12          | `\|>` (pipeline)                                                  | left          |
| 13          | `+>` (composition)                                                | left          |
| 14          | `=` `+=` `-=` `*=` `/=` `^=` `%=` `&&=` `\|\|=` `~^=` `<<=` `>>=` | right         |
| 15 (lowest) | `if ?? else`                                                      | right         |

> [!NOTE]
> **`if_expr` precedence:** `if cond ?? thenExpr else elseExpr` begins with the `if` keyword. The `??` here is a fixed syntactic separator in the if-expression production, not the null-coalescing infix operator (level 11). The `else` clause is right-associative at the lowest precedence — chained `if ?? ... else if ?? ... else ...` forms bind correctly.

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

Raw string literals use `r` followed by zero or more `#` characters, then a double quote, the literal content, a double quote, and the same number of `#` characters. The content is taken verbatim — no escape sequences are processed.

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

Attribute arguments are intentionally limited to compile-time literals and type identifiers. Runtime expressions are not valid inside attribute arguments.

#### Known Attributes

| Attribute                | Valid on                | Purpose                                   |
| ------------------------ | ----------------------- | ----------------------------------------- |
| `@extern("sym")`         | `let`, `const` func/var | Bind to C/OS/Vulkan symbol                |
| `@extern("sym", "conv")` | `let`, `const` func/var | With explicit calling convention          |
| `@inline`                | func                    | Suggest always inline                     |
| `@noinline`              | func                    | Prevent inlining                          |
| `@packed`                | `struct`                | Remove padding — all fields byte-adjacent |
| `@deprecated("msg")`     | func, var, struct       | Emit warning at every use site            |
| `@phantom`               | `type` alias            | Allow unused generic parameters           |
| `@aot`                   | `main` only             | Ahead-of-time compilation                 |
| `@jit`                   | `main` only             | JIT compilation                           |

`@inline` and `@noinline` are mutually exclusive on the same declaration.
`@aot` and `@jit` are mutually exclusive on the same declaration.

#### `@extern` Rules

- Requires `const`, not `let` — the linker resolves the symbol permanently.
- Functions must have no body.
- `W3001` warning when `let` is used instead of `const`.
- `W3002` warning when an empty body `= {}` is supplied (body silently ignored).
- `E3002` error when a non-empty body is supplied.

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

#### `@phantom` Rules

- Only valid on `type` alias declarations that have generic parameters.
- Without `@phantom`, a type parameter that does not appear in the alias body is a **compile error**.
- With `@phantom`, unused parameters are permitted — the compiler treats them as phantom tags that exist only at the type-checking level and are erased at runtime.
- Phantom parameters still participate in type identity: `Tagged<int>` and `Tagged<string>` are distinct types even though both resolve to `int` at runtime.

```luc
@phantom
type Tagged<T> = int    -- OK: T is a phantom tag

type Gen<T> = int       -- ERROR: T is unused — add @phantom if intentional
```

---

### Compiler Intrinsics (`#`)

Intrinsic calls appear in expression position and are prefixed with `#`. Unlike attributes, intrinsics can take runtime expressions and types as arguments.

```
intrinsic_call  := '#' IDENTIFIER '(' [ intrinsic_arg_list ] ')'

intrinsic_arg_list := intrinsic_arg { ',' intrinsic_arg }

intrinsic_arg   := expr
                 | type
```

#### Compile-Time Type Queries

| Intrinsic     | Returns  | Notes                                              |
| ------------- | -------- | -------------------------------------------------- |
| `#sizeof(T)`  | `uint64` | Byte size of type T — compile-time constant        |
| `#alignof(T)` | `uint64` | Alignment requirement of T — compile-time constant |

```luc
let size  uint64 = #sizeof(Vertex)
let align uint64 = #alignof(Vec2)
```

#### Floating-Point Math

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
let hyp     float = #sqrt(x*x + y*y)
let rounded float = #round(value)
let maxVal  int   = #min(a, b)
```

#### Bit Manipulation (Integer Types Only)

| Intrinsic      | Args    | Returns | Notes                           |
| -------------- | ------- | ------- | ------------------------------- |
| `#clz(x)`      | integer | same    | Count leading zero bits         |
| `#ctz(x)`      | integer | same    | Count trailing zero bits        |
| `#popcount(x)` | integer | same    | Count set (1) bits              |
| `#bswap(x)`    | integer | same    | Reverse byte order (endianness) |

```luc
let leading  uint32 = #clz(flags)
let trailing uint32 = #ctz(flags)
let bits     uint32 = #popcount(mask)
let swapped  uint32 = #bswap(networkOrder)
```

#### Memory Operations

| Intrinsic                 | Args               | Returns | Notes                       |
| ------------------------- | ------------------ | ------- | --------------------------- |
| `#memcpy(dst, src, len)`  | ptr, ptr, uint64   | void    | Copy bytes, no overlap      |
| `#memmove(dst, src, len)` | ptr, ptr, uint64   | void    | Copy bytes, handles overlap |
| `#memset(dst, val, len)`  | ptr, ubyte, uint64 | void    | Fill bytes with value       |

All memory intrinsics operate on raw pointers (`*T`) and are only valid inside `@extern`-decorated functions or other intrinsic calls.

```luc
#memcpy(dest, src, #sizeof(Buffer))
#memset(ptr, 0, size)
```

#### Pointer Operations

| Intrinsic            | Args       | Returns | Notes                                 |
| -------------------- | ---------- | ------- | ------------------------------------- |
| `#ptrToRef(ptr)`     | `*T`       | `&T`    | Assert valid, cross to safe reference |
| `#refToPtr(ref)`     | `&T`       | `*T`    | Convert reference to raw pointer      |
| `#ptrOffset(ptr, n)` | `*T`, int  | `*T`    | Pointer arithmetic (element offset)   |
| `#ptrDiff(p1, p2)`   | `*T`, `*T` | `int64` | Distance between pointers in elements |

These intrinsics are the only way to cross the sealed conduit boundary or perform pointer arithmetic.

```luc
let buf  *uint8 = malloc(1024)
let ref  &uint8 = #ptrToRef(buf)
ref = 0xFF

let next     *uint8 = #ptrOffset(buf, 1)
let distance  int64 = #ptrDiff(next, buf)
```

#### Unsafe / Bit Reinterpretation

| Intrinsic        | Args        | Returns | Notes                                             |
| ---------------- | ----------- | ------- | ------------------------------------------------- |
| `#bitcast(T, x)` | type, value | `T`     | Reinterpret bits of x as type T; sizes must match |

Valid only inside `@extern`-decorated functions or when the compiler flag `--unsafe` is enabled.

```luc
let bits uint32  = 0x3F800000
let f    float32 = #bitcast(float32, bits)   -- 1.0
```