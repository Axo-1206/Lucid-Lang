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

### Shared Productions

The following productions are used across multiple sections. They are defined once here and referenced by name throughout:

```
-- A named function reference — used in impl assignment, from path entry,
-- pipeline steps, and composition operands.
-- func_ref may be plain or carry qualifiers (~async, ~nullable, ~parallel).
-- The full resolved type including qualifiers is what the caller sees.
-- func_ref must be a named reference — NOT a call expression, NOT a
-- factory function whose return value is a function, NOT a partial
-- application result, NOT an anonymous function literal.
-- @extern functions are allowed if non-variadic.
func_ref    := IDENTIFIER                      -- local or imported name
             | IDENTIFIER '.' IDENTIFIER       -- module path: pkg.fn
             | func_ref generic_args           -- generic instantiation: fn<T>
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
-- [_, string]: slice — the runtime owns the argument buffer, main gets a read-only view
-- [*, string] would be wrong: that implies main owns a heap copy of all arguments
export const main (args [_, string]) -> int = {
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

### No Classes (Encapsulation via Modules)

Luc does not feature a `class` keyword. Encapsulation is handled entirely at the module (file) level using visibility modifiers, while data layout and behavior are kept strictly separate:
*   **Data:** Declared via simple, flat data structures (`struct` and `enum`).
*   **Behavior:** Declared via functions and `impl` blocks.
*   **Encapsulation:** Visibility rules (`pub` / `export`) control what elements are exposed outside the module.

This supports a **Data-Oriented Design (DOD)** philosophy, separating memory layout from logic to maximize CPU cache efficiency and simplify application state.

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
array_type      := '[' '_' ',' type ']'              -- slice:   [_, T]  (fat pointer view)
                 | '[' '*' ',' type ']'              -- dynamic: [*, T]  (heap-owned, growable)
                 | '[' INT_LITERAL ',' type ']'      -- fixed:   [N, T]  (stack, compile-time size)

-- Generic array type — valid as an impl target or a from target.
-- '<' IDENTIFIER '>' inside the bracket declares a free type variable.
-- The compiler unifies the variable with the concrete element type at the declaration site.
-- Equivalent to declaring a type alias 'type Slice<T> = [_, T]' and using 'impl Slice<T>'
-- or 'from Slice<T>'.
-- Only one type variable per array type — arrays have one element type.
-- IMPORTANT: generic_array_type is ONLY valid as an impl or from target — it is NOT a valid
-- type in variable declarations, function parameters, return types, or struct fields.
-- Use a type alias for those contexts: type Slice<T> = [_, T]
generic_array_type := '[' '_' ',' '<' IDENTIFIER '>' ']'         -- [_, <T>]
                    | '[' '*' ',' '<' IDENTIFIER '>' ']'         -- [*, <T>]
                    | '[' INT_LITERAL ',' '<' IDENTIFIER '>' ']' -- [N, <T>]

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
| Fixed array    | `[N, T]`                         | stack / inline                  | full element copy                  |
| Slice          | `[_, T]`                         | fat pointer (`ptr + len + cap`) | copies view header — shares buffer |
| Dynamic array  | `[*, T]`                         | heap-owned buffer               | full deep copy                     |
| String         | `string`                         | heap-owned sequence             | full deep copy                     |
| Struct         | `Vec2` `Player` …                | inline / stack                  | full deep copy                     |
| Named function | `add` `myVector:normalize`       | function pointer                | pointer copy                       |
| Closure        | `add(10)` `(x int) -> int { … }` | heap-allocated env              | copies reference to env            |

### Struct Deep Copy

Struct assignment always produces a fully independent value. Owned fields are cloned. Since references (`&T`) cannot be stored in structs (see Scoped Reference Rules below), structs consist entirely of owned fields and deep copies are always independent.

```luc
struct Player {
    score  int        -- owned: cloned
    items  [*, string]  -- owned: buffer deep-copied
}

let a Player = Player { score = 10  items = ["sword"] }
let b Player = a
-- b.score and b.items are fully independent of a
```

### Borrowed Types — Scoped References (`&T`)

References (`&T`) allow sharing data without copying. They represent a safe borrowed view of an owned variable.

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

#### The Downward Flow Rule (Reference Scoping)

To guarantee memory safety and eliminate dangling pointers without using a Garbage Collector or a complex compile-time borrow checker, references (`&T`) are strictly scoped. They are allowed to flow *downward* (into nested calls), but never *upward or sideways*.

1. **No Struct Storage:** A struct field cannot have a reference type (e.g., `field &T` is a compile error).
2. **No Array/Slice Storage:** An array or slice cannot store reference types (e.g., `[*, &T]` or `[_, &T]` is a compile error).
3. **No Reference Returns:** A function cannot return a reference type (e.g., `-> &T` is a compile error).

As a result, a reference (`&T`) can only exist in two places:
*   As a **function parameter** (e.g., `let process(p &Player)`).
*   As a **local variable alias** inside a block (e.g., `let ref &Weapon = player.weapon`).

This guarantees that a reference never outlives the owned variable it points to.

#### Modeling Complex Data Structures (Trees, Graphs, Links)

Because references (`&T`) cannot be stored inside structs, building circular or linked data structures requires alternative approaches:

1. **Indices and Arenas (Recommended DOD Style):** Store integer indices into a flat array/arena instead of memory references.
   ```luc
   struct Node {
       value int
       next  int    -- index of the next node in an array / arena
   }
   ```
2. **Raw Pointers (`*T`):** For manual memory management (e.g., building low-level systems or C integrations), use raw pointers. Raw pointers are "sealed conduits" and require explicit `#toRef` conversion to use, signaling unsafe operations. Type conversion syntax (e.g., `*T(val)` or `&T(val)`) is forbidden for raw pointers and references; the `#toRef` and `#toPtr` intrinsics must be used instead to cross the unsafe boundary.
   ```luc
   struct Node {
       value int
       next  *Node?  -- raw pointer, nullable. Requires manual lifecycle tracking.
   }
   ```
3. **Smart Pointers (Standard Library):** For safe shared heap state, use standard library reference-counted wrappers like `Shared<T>` and `Weak<T>` (which auto-nulls when the owner is destroyed). These incur a small runtime cost.

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
4. Pass to pointer intrinsics (`#toRef`, `#ptrOffset`, etc.)
5. Print the address for debugging

### Forbidden Operations (Compiler Error)

- Dereferencing: `*ptr`
- Field access: `ptr.field`
- Indexing: `ptr[i]`
- Arithmetic: `ptr + 4` — use `#ptrOffset` instead
- Assignment: `*ptr = value`
- Type casting/conversion: e.g., `*float(x)` or `&float(x)` (must use `#toRef` or `#toPtr` to cross safety boundaries)

### Boundary Crossing (Intrinsics)

```luc
#toRef(ptr)         -- *T → &T  (assert validity, cross to safe reference)
#toPtr(ref)         -- &T → *T  (convert back to raw pointer)
#ptrOffset(ptr, n)  -- pointer arithmetic, returns new *T
#ptrDiff(p1, p2)    -- distance between two pointers as int64
```

```luc
@extern("malloc")
const malloc (size uint64) -> *uint8?

let buf *uint8? = malloc(1024)
if buf == nil { return 1 }

let ref &uint8 = #toRef(buf)           -- cross the boundary
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
- **Heap-allocated types** (`string`, `[*, T]`, **closures**) are cleaned up when the variable goes out of scope.
- **Destructors and the `Disposable` Trait:** Types can implement the standard library `Disposable` trait to run custom cleanup logic when they go out of scope.
  - Built-in heap types (`string`, `[*, T]`) have internal disposers generated automatically.
  - Custom types that manage external resources (like raw pointers `*T` or file handles) implement `Disposable` to free their resources automatically.
  - To free resources **early** while a scope is still live, the developer can explicitly call `.dispose()` (or `.clear()` for collections).

A plain named function declared inside a scope is just a function pointer — no heap allocation. A **closure** (an anonymous function that captures variables from the enclosing scope) allocates its captured environment on the heap. That environment is released when the closure variable goes out of scope:

```luc
let factor int = 3

-- plain function — just a pointer, no heap allocation
let double (x int) -> int = { return x * 2 }

-- closure — captures 'factor', heap-allocates the environment
let triple (x int) -> int = { return x * factor }
-- triple's captured environment is released when triple goes out of scope

-- Custom disposable struct
struct File {
    handle int
}

impl Disposable for File {
    pub const dispose (self &File) = {
        closeFile(self.handle) -- Automatically called at scope exit
    }
}
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
let parallelFor<T> ~parallel (items [*, T], body (item T)) = { ... }
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

The inverse narrowing early-exit pattern also applies to `~nullable` function variables — including mixed conditions with `or`:

```luc
let process (handler ~nullable (e Event), ctx Context?) -> int = {
    if handler == nil or ctx == nil { return -1 }
    -- handler is narrowed to plain (e Event)
    -- ctx is narrowed to Context
    handler(someEvent)    -- OK: no nil check needed
    return ctx.id
}

**`~parallel` — body restrictions:**

When a function is called through a `~parallel`-qualified binding, the compiler enforces these restrictions inside the body function argument:

- No `return` statements
- No `break` or `continue`
- No `await` expressions
- No writes to variables declared outside the body scope

```luc
let parallelFor<T> ~parallel (items [*, T], body (item T)) = { ... }

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

Multiple parameter groups express curry. Each `()` group is one call step. See **`## Curry Functions`** for the full specification of both forms, execution semantics, partial application, and performance characteristics.

```luc
let add (a int)(b int) -> int = { return a + b }

add(10)       -- returns (b int) -> int
add(10)(5)    -- 15

-- chains with |> and +>
let addTen (b int) -> int = add(10)
42 |> addTen                   -- 52

let process (b int) -> string = add(10) +> string
process(5)                     -- "15"
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

Generic functions allow code to operate on multiple types using type parameters declared between `<` and `>` immediately after the function name — before any qualifiers or parameter groups.

```
generic_params  := '<' generic_param { [','] generic_param } '>'

generic_param   := IDENTIFIER
                 | IDENTIFIER ':' IDENTIFIER
                 | IDENTIFIER ':' constraint_list

constraint_list := IDENTIFIER { '+' IDENTIFIER }
```

#### Declaration

```luc
-- single type parameter
let identity<T> (v T) -> T = { return v }

-- multiple type parameters
let map<T, U> (items [*, T], f (item T) -> U) -> [*, U] = { ... }

-- type parameter with constraint
let printSorted<T : Comparable + Printable> (items [_, T]) = { ... }

-- qualifier comes after generic params
let fetch<T> ~async (url string) -> T = { ... }

-- curry with generics
let wrap<T> (v T)(label string) -> string = {
    return label + ": " + string(v)
}
```

#### Type Constraints

Constraints restrict what types a parameter accepts. A type parameter with no constraint accepts any type. A type parameter with a constraint only accepts types whose `impl` satisfies the named trait:

```luc
-- T must satisfy Comparable
let max<T : Comparable> (a T, b T) -> T = {
    if a:compareTo(b) >= 0 { return a }
    return b
}

-- T must satisfy both Comparable and Printable
let printMax<T : Comparable + Printable> (a T, b T) = {
    let m T = max<T>(a, b)
    m:print()
}

-- multiple independent constraints
let transfer<S : Readable, D : Writable> (src S, dst D) = { ... }
```

#### Instantiation

Generic functions must always be called with explicit type arguments. Luc does not infer type arguments:

```luc
let x int    = identity<int>(42)
let s string = identity<string>("hello")

let nums [*, int]    = map<int, int>([1, 2, 3], (n int) -> int { return n * 2 })
let strs [*, string] = map<int, string>([1, 2, 3], (n int) -> string { return string(n) })
```

A generic function can also be assigned to a concrete non-generic variable by instantiating it at the assignment site. The resulting variable is a plain function — no type arguments needed at the call site:

```luc
export const sum<T : Numeric> (a T, b T) -> T = { return a + b }

-- instantiate at assignment: sumOfInt is now a plain (int, int) -> int
let sumOfInt (n1 int, n2 int) -> int = sum<int>

-- call site: no type args needed
let foo int = sumOfInt(2, 3)    -- 5

-- also works with curry
export const clampTo<T : Comparable> (lo T)(hi T)(v T) -> T = { ... }

let clampInt (lo int)(hi int)(v int) -> int = clampTo<int>
let result int = clampInt(0)(100)(42)    -- 42
```

> [!NOTE]
> No type inference for generic functions — type must always be specified at the instantiation site, whether that is a call `identity<int>(42)` or an assignment `let f = identity<int>`.

#### Generic Functions and Currying

Generic parameters interact cleanly with curry — the type parameter applies to all parameter groups:

```luc
let clampTo<T : Comparable> (lo T)(hi T)(v T) -> T = {
    if v:compareTo(lo) < 0 { return lo }
    if v:compareTo(hi) > 0 { return hi }
    return v
}

let clamp0to100 = clampTo<int>(0)(100)    -- (v int) -> int
clamp0to100(42)                           -- 42
clamp0to100(150)                          -- 100

-- pipelines
42  |> clampTo<int>(0)(100)!    -- 42
150 |> clampTo<int>(0)(100)!    -- 100
```

#### Generic Functions and Arrays

Generic functions work naturally with all three array kinds:

```luc
-- works on slice, dynamic, or fixed via type alias
let sum<T : Numeric> (items [_, T]) -> T = {
    let total T = 0
    for v T in items { total += v }
    return total
}

let filter<T> (items [_, T], pred (v T) -> bool) -> [*, T] = {
    let result [*, T] = []
    for v T in items {
        if pred(v) { result:push(v) }
    }
    return result
}

-- call
let nums [_, int] = [1, 2, 3, 4, 5]
let evens [*, int] = filter<int>(nums, (v int) -> bool { return v % 2 == 0 })
```

> [!WARNING]
> Generic functions cannot be stored in arrays or assigned to plain function variables without instantiation. A generic function is not a value until its type parameters are resolved:
>
> ```luc
> type IntFn = (int) -> int
>
> let arr [_, IntFn] = [identity]           -- ERROR: identity is generic
> let arr [_, IntFn] = [identity<int>]      -- OK: instantiated
> let f    IntFn     = identity             -- ERROR: generic without type args
> let f    IntFn     = identity<int>        -- OK
> ```

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
let find  ~nullable (items [*, int], pred (item int) -> bool) -> int = { ... }

-- generic (generic before qualifiers, always)
let map<T, U> (items [*, T], f (item T) -> U) -> [*, U] = { ... }

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

## Curry Functions

A curry function is a function that can be partially applied — called with fewer argument groups than it declares, producing a new function that accepts the remaining groups. Luc supports two syntactic forms for curry functions. Both produce the same call site but have different semantics, performance characteristics, and expressiveness.

### Form 1 — Explicit Return Function

The outer function explicitly returns an inner function as its body result. The return type in the signature names the inner function type:

```luc
let add (a int) -> (b int) -> int = {
    return (b int) -> int { return a + b }
}
```

**What executes when:**

The outer body runs immediately when the first group is called. The inner function is created at that point and returned as a value:

```luc
let addTen (b int) -> int = add(10)
-- outer body ran: closure created capturing a=10
-- inner function returned and stored in addTen

let result int = addTen(5)    -- 15: inner body runs here
```

**Pre-computation at partial application:**

Because the outer body runs at the first call, you can do expensive setup once and capture the result — the inner function then uses the already-computed value:

```luc
let makeProcessor (config Config) -> (data string) -> string = {
    let compiled = config:compile()    -- runs ONCE at makeProcessor(config)
    return (data string) -> string { return compiled:apply(data) }
}

let process (data string) -> string = makeProcessor(myConfig)
-- config:compile() already done — process(data) is cheap on every call
process("input1")
process("input2")
```

**Explicit capture control:**

The inner function captures only what you explicitly reference. You can transform outer parameters before capturing:

```luc
let scaleBy (factor int) -> (v int) -> int = {
    let multiplier int = factor * 2    -- transform before capture
    return (v int) -> int { return v * multiplier }
    -- multiplier captured, not factor
}
```

### Form 2 — Multiple Parameter Groups

Multiple `()` groups after the function name. The body runs only when **all** groups are provided. The compiler desugars this into form 1 internally — you never write the nested return:

```luc
let add (a int)(b int) -> int = {
    return a + b    -- a and b both available: body runs when both groups provided
}
```

**What executes when:**

The body does not run until every parameter group is supplied. Partial application produces a compiler-generated closure:

```luc
let addTen (b int) -> int = add(10)
-- body has NOT run yet: compiler wrapped add with a=10 captured
-- addTen is a closure waiting for b

let result int = addTen(5)    -- body runs NOW: a=10, b=5 → 15
```

**Deep curry:**

```luc
let clamp (lo int)(hi int)(v int) -> int = {
    if v < lo { return lo }
    if v > hi { return hi }
    return v
}

let clamp0to100 (v int) -> int = clamp(0)(100)
clamp0to100(42)     -- 42
clamp0to100(150)    -- 100
clamp0to100(-5)     -- 0
```

**Generic parameters apply to all groups:**

```luc
let map<T, U> (items [_, T])(f (T) -> U) -> [*, U] = {
    let result [*, U] = []
    for item T in items { result:push(f(item)) }
    return result
}

let doubled [*, int] = map<int, int>([1, 2, 3])(double)
let strs     [*, string] = map<int, string>([1, 2, 3])(toString<int>)
```

### Key Differences

|                             | Form 1                                          | Form 2                                   |
| --------------------------- | ----------------------------------------------- | ---------------------------------------- |
| Syntax                      | `(a int) -> (b int) -> int`                     | `(a int)(b int) -> int`                  |
| Body runs at                | First group call                                | All groups provided                      |
| Pre-computation             | ✅ Before inner function created                 | ❌ Not possible                           |
| Capture control             | ✅ Explicit — reference what you need            | ❌ Implicit — compiler captures used vars |
| Boilerplate                 | Higher — must write `return (...) -> T { ... }` | Lower — plain body                       |
| Mixing logic between groups | ✅ Yes — outer body is free code                 | ❌ No — only one body for all groups      |
| Compiler desugars to        | Itself                                          | Form 1                                   |

### Partial Application — Intermediate Type Annotation Required

Luc requires explicit type annotations on all declarations — partial application is no exception. When storing an intermediate result, you must write the full type of the remaining function:

```luc
let add (a int)(b int) -> int = { return a + b }

-- CORRECT: explicit type annotation on the partial result
let addTen (b int) -> int = add(10)

-- ERROR: no type annotation
let addTen = add(10)    -- ERROR: type annotation required
```

The type annotation documents what the partial application produces and makes the remaining parameter groups explicit to the reader.

### Mixing Forms

You can mix form 1 and form 2 in the same declaration — form 2 groups come first, followed by a form 1 explicit return:

```luc
-- form 2 for first two groups, form 1 for the third
let f (a int)(b int) -> (c int) -> int = {
    let sum int = a + b              -- runs when both a and b are provided
    return (c int) -> int { return sum + c }
}

-- partial applications
let f10_20 (c int) -> int = f(10)(20)   -- sum=30 computed here
f10_20(5)                               -- 35
```

### Recursion

Both forms support recursion. In form 2, the recursive call provides all groups:

```luc
let power (base int)(exp int) -> int = {
    if exp == 0 { return 1 }
    return base * power(base)(exp - 1)    -- all groups provided
}

power(2)(10)    -- 1024
```

In form 1, recursion can appear in either the outer body or the inner function:

```luc
let countdown (n int) -> (step int) -> int = {
    if n <= 0 { return (step int) -> int { return 0 } }
    return (step int) -> int { return n + countdown(n - step)(step) }
}
```

### Curry in `impl`

Methods declared with multiple parameter groups in an `impl` block follow the same rules. The receiver (`self` or `as` alias) is always the implicit zeroth argument — separate from the parameter groups:

```luc
impl int as i {
    add (b int)(c int) -> int = { return i + b + c }
}

-- call site
let result int = 5:add(3)(2)    -- i=5, b=3, c=2 → 10

-- partial application — receiver already bound
let add3 (c int) -> int = 5:add(3)    -- i=5, b=3 captured
add3(2)                                -- 10
```

### Curry and Pipelines / Composition

Curry functions are **forbidden** as direct `|>` steps or `+>` operands — the upstream value fills only the first group, leaving remaining groups unresolvable. Pre-apply all but the last group first:

```luc
-- ERROR: curry function as pipeline step
42 |> add        -- ERROR: add has 2 groups

-- CORRECT: pre-apply to produce a single-group function
let addTen (b int) -> int = add(10)
42 |> addTen     -- OK: addTen has one group

-- CORRECT: same for +>
let process (b int) -> string = add(10) +> string
```

### Performance

| Usage                           | Form 1                      | Form 2                       |
| ------------------------------- | --------------------------- | ---------------------------- |
| Full application `add(1)(2)`    | Inlined — zero overhead     | Inlined — zero overhead      |
| Partial stored `let f = add(1)` | Heap closure                | Heap closure                 |
| Pre-computation at partial      | ✅ Runs once at partial      | ❌ Repeats on every full call |
| LLVM escape analysis            | Stack-alloc if non-escaping | Stack-alloc if non-escaping  |
| Inline `add(1)(2)` in pipeline  | LLVM eliminates closure     | LLVM eliminates closure      |

For pure functions with no setup work, both forms have identical runtime performance. Form 1 only outperforms form 2 when the outer body performs computation that should run once at the partial application boundary.

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
    objects [_, T]
}

struct Cache<K : Hashable + Comparable, V> {
    keys   [_, K]
    values [_, V]
}

-- struct literals
let origin Vec2  = Vec2  { x = 0.0  y = 0.0 }
let white  Color = Color {}                     -- all fields take defaults
let p Vec2<int>  = Vec2<int> { x = 1  y = 2 }

-- field access
let x float = origin.x    -- read
origin.x = 5.0            -- write (only valid if origin is 'let')
```

### Generic Structs

A struct can declare one or more type parameters between `<` and `>` immediately after its name. Every type parameter must appear at least once in the field declarations — unused parameters require `@phantom`.

#### Declaration

```luc
-- single type parameter
struct Box<T> {
    value T
}

-- multiple type parameters
struct Pair<K, V> {
    first  K
    second V
}

-- constrained type parameter
struct SortedPair<T : Comparable> {
    lo T
    hi T
}

-- multiple constraints
struct Index<K : Hashable + Comparable, V> {
    keys   [_, K]
    values [_, V]
}

-- type parameter used in array fields
struct Stack<T> {
    items    [*, T]
    capacity uint64
}

-- nullable field using type parameter
struct Result<T, E> {
    value T?
    error E?
}
```

#### Instantiation

Generic structs must always be instantiated with explicit type arguments. Luc does not infer type arguments for struct literals:

```luc
let box     Box<int>              = Box<int>    { value = 42 }
let pair    Pair<string, int>     = Pair<string, int>  { first = "age"  second = 30 }
let stack   Stack<float>          = Stack<float> { items = []  capacity = 16 }
let result  Result<int, string>   = Result<int, string> { value = 5  error = nil }
```

#### Generic Structs and `impl`

An `impl` block for a generic struct must declare matching generic parameters. The parameter names are independent — they bind positionally:

```luc
struct Box<T> { value T }

impl Box<T> as b {
    get     ()  -> T    = { return b.value }
    isEmpty ()  -> bool = { return b.value == nil }
}

-- instantiation at call site
let box Box<int> = Box<int> { value = 42 }
box:get()       -- 42
```

#### Nesting Generic Structs

Generic structs can be used as field types in other generic structs:

```luc
struct Node<T> {
    value T
    next  Node<T>?    -- recursive: nullable to allow termination
}

struct Tree<T : Comparable> {
    root Node<T>?
    size uint64
}

-- instantiation
let list Node<int> = Node<int> { value = 1  next = nil }
```

#### Generic Struct and Default Field Values

Default values on generic fields are allowed only when the expression is valid for all possible instantiations — typically `nil` for nullable fields or zero literals for numeric parameters:

```luc
struct Container<T> {
    value    T?       = nil     -- OK: nil is valid for any T?
    count    uint64   = 0       -- OK: 0 is a concrete literal
    -- label T        = ""      -- ERROR: "" is not valid for all T
}
```

> [!NOTE]
> A generic struct with all fields defaulted still requires explicit type arguments at the instantiation site — the compiler cannot infer `T` from an empty literal:
>
> ```luc
> let c = Container {}           -- ERROR: type arguments required
> let c Container<string> = Container<string> {}   -- OK: all fields take defaults
> ```

> [!WARNING]
> Every type parameter must be used in at least one field. An unused parameter is a compile error unless the struct is annotated with `@phantom`:
>
> ```luc
> struct Wrapper<T> {
>     raw int    -- ERROR: T is unused
> }
>
> @phantom
> struct Tagged<T> {
>     raw int    -- OK: T is a phantom tag, erased at runtime
>     -- Tagged<int> and Tagged<string> are distinct types despite identical layout
> }
> ```

### Independent Generics in Struct Fields

Struct fields **cannot** have independent type parameters — parameters that are not declared on the struct itself. Every type parameter used in a field must be resolvable when the struct is instantiated.

**Why:** Every field must have a known size at instantiation time so the compiler can compute the struct's memory layout. The struct's own type parameters (`T` in `Foo<T>`) are resolved at instantiation — `Bar<T>` becomes `Bar<int>` when you write `Foo<int>`. An independent `<U>` with no resolution site has no knowable size and cannot be laid out in memory.

```luc
struct Foo<T> {
    value    T           -- OK: T from struct, resolved at Foo<int>
    items    [_, T]      -- OK: T from struct, array element type resolved
    wrapped  Bar<T>      -- OK: T from struct, Bar<int> when Foo<int>
    process  (v T) -> T  -- OK: function field, T from struct
                         --     function pointer is fixed size regardless of T

    nested   Bar<int>    -- OK: concrete argument, fully resolved at declaration

    bad      Bar<U>      -- ERROR: U is unknown — not declared on struct
                         --        no instantiation site to resolve U
                         --        compiler cannot determine size of this field
}
```

**Function type fields are a special case:**

A function type field stores a function pointer — fixed size (one pointer width) regardless of the function's type parameters. So `(v T) -> T` in a struct field is valid — `T` is from the struct and the pointer size is always known:

```luc
struct Transformer<T> {
    fn  (v T) -> T      -- OK: function pointer, fixed size
                        --     T resolved from Transformer<T> at instantiation
}

let t Transformer<int> = Transformer<int> { fn = double }
t.fn(21)    -- 42: T = int, fn is (int) -> int
```

This is distinct from `impl` methods, which are not stored in the struct and can freely use independent generics — because methods are function pointers resolved at call time, not values laid out in memory.

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
type Wrap<T>  = [_, T]                         -- OK: T is used
type Gen<T>   = int                            -- ERROR: T is unused
type Odd<T>   = string                         -- ERROR: T is unused
```

If you intentionally want a type parameter that does not appear in the body (a phantom type), annotate with `@phantom`. This also applies to generic structs and generic functions — see the `@phantom` Rules section under Known Attributes.

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

| Kind                         | Convention                 | Example                        |
| ---------------------------- | -------------------------- | ------------------------------ |
| Slice (`[_, T]`)             | `...List` suffix           | `IntList`, `UserList`          |
| Dynamic array (`[*, T]`)     | `...Array` suffix          | `IntArray`, `UserArray`        |
| Fixed array (`[N, T]`)       | `...Buffer` suffix         | `ByteBuffer`, `FloatBuffer`    |
| Generic slice (`[_, <T>]`)   | `List<T>` — no prefix      | `List<T>`, `List<User>`        |
| Generic dynamic (`[*, <T>]`) | `Array<T>` — no prefix     | `Array<T>`, `Array<User>`      |
| Generic fixed (`[N, <T>]`)   | `Buffer<T, N>` — no prefix | `Buffer<T, 256>`               |
| Nullable (`T?`)              | `Maybe...` prefix          | `MaybeInt`, `MaybeUser`        |
| Result (`T!E`)               | `...Or...` — both sides    | `IntOrString`, `UserOrDbError` |
| Function type                | `...Fn` suffix             | `ParserFn`, `HandlerFn`        |
| Nullable function            | `Maybe...Fn`               | `MaybeParserFn`                |
| Async function               | `...AsyncFn` suffix        | `FetchAsyncFn`                 |
| Parallel function            | `...ParallelFn` suffix     | `TransformParallelFn`          |

The `Or` convention for result types surfaces both the success and error type explicitly. Generic aliases follow the same base suffix as their concrete counterparts — the `<T>` already signals the kind, so no prefix like `Generic` or `T` is needed or wanted.

> [!TIP]
> These are conventions only — the compiler does not enforce them. Combinations chain naturally: `MaybeUserOrDbError` (nullable success, can fail), `UserListOrDbError` (array that can fail).

### Generic Naming Convention

When naming a generic type alias, the rule is simple: **use the same name you would for the concrete version, just add `<T>`**. Do not prefix or suffix with `Generic`, `T`, or any other marker — the angle brackets already communicate that the alias is generic.

```luc
-- concrete aliases
type UserList   = [_, User]       -- slice of User
type IntArray   = [*, int]        -- dynamic array of int
type ByteBuffer = [256, byte]     -- fixed buffer of bytes

-- generic counterparts — same name, add <T>
type List<T>      = [_, T]        -- NOT TList, NOT GenericList
type Array<T>     = [*, T]        -- NOT TArray, NOT GenericArray
type Buffer<T, N> = [N, T]        -- NOT TBuffer, NOT GenericBuffer

-- generic and concrete read the same way at use sites
let users  List<User>  = []
let users  UserList    = []       -- equivalent concrete form
```

Generic aliases for function types and structs follow the same principle:

```luc
-- concrete function alias
type ParserFn    = (src string) -> int

-- generic function alias — same base name, add <T>
type ParserFn<T> = (src string) -> T    -- NOT TParserFn, NOT GenericParserFn

-- struct aliases are already generic by nature — no extra marker needed
type Pair<K, V>                 = struct { first K  second V }
type SortedPair<T : Comparable> = struct { first T  second T }
```

The only time a qualifier word is appropriate is when the constraint is semantically meaningful and the name without it would be misleading:

```luc
-- 'Comparable' is useful here — it tells the reader this list guarantees ordering
type ComparableList<T : Comparable> = [_, T]

-- 'Generic' would be useless — all lists with <T> are already obviously generic
type GenericList<T> = [_, T]    -- bad: Generic adds nothing
```

### Independent Generics in Type Alias Bodies

A type alias body may only reference type parameters declared on the alias itself. Independent parameters — ones used in the body but not declared — are a compile error. The alias body must be fully resolvable from its own declared parameters:

```luc
-- OK: T declared and used
type Transform<T>    = (v T) -> T
type Converter<T, U> = (v T) -> U       -- OK: both T and U declared

-- ERROR: U appears in body but is not declared on the alias
type Bad<T>          = (v T) -> U       -- ERROR: U unknown
type AlsoBad<T>      = [_, U]           -- ERROR: U unknown

-- CORRECT: declare U on the alias
type Converter<T, U> = (v T) -> U       -- OK
```

This mirrors the struct field rule — an alias body must be fully resolvable to a concrete shape when all its declared parameters are provided. An undeclared `U` has no resolution site and produces an unresolvable shape.

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

-- concrete array aliases
type UserArray  = [*, User]
type UserList   = [_, User]
type ByteBuffer = [256, byte]

-- generic array aliases — same base name, add <T>
type List<T>      = [_, T]
type Array<T>     = [*, T]
type Buffer<T, N> = [N, T]

-- array result — alias the array first, then apply Or convention
type UserArrayOrDbError  = UserArray!DbError
type ListOrDbError<T>    = List<T>!DbError    -- generic result alias

-- function aliases
type ParserFn            = (src string) -> int
type ParserFn<T>         = (src string) -> T      -- generic variant
type FetchAsyncFn        = ~async    (url string) -> string
type TransformParallelFn = ~parallel (item Vec2)  -> Vec2

-- function result — alias the function first, then apply Or convention
type FetchAsyncFnOrNetError = FetchAsyncFn!NetworkError

-- nullable function alias
type MaybeParserFn = ~nullable (src string) -> int

-- array of function type
type HandlerFn        = (event Event) -> bool
type HandlerFnList    = [_, HandlerFn]          -- concrete: slice of handlers
type HandlerFnArray   = [*, HandlerFn]          -- concrete: dynamic array of handlers
type HandlerFnList<T> = [_, (event T) -> bool]  -- generic variant

-- generic struct alias
type Pair<K, V>   = struct { first K  second V }
type Transform<T> = (v T) -> T

-- constrained generic alias
type SortedPair<T : Comparable>     = struct { first T  second T }
type ComparableList<T : Comparable> = [_, T]
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
- **All types except function types can implement a trait.** This includes structs, primitives, enums, type aliases, and array types.
- A type satisfies a trait by declaring `impl TypeName : TraitName { ... }`.
- The impl declaration must provide every method in the trait.
- No function overloading — each method name must be unique.

### Trait Conformance — All Types Except Function Types

Trait conformance is valid for every type that can be an `impl` target, with the single exception of function types. Function types are excluded because adding trait conformance to a function type blurs the boundary between values and objects — every function of that exact signature would implicitly gain the trait's methods, which is surprising and conflicts with Luc's design where functions are pure values with no method dispatch.

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

pub trait Iterable<T> {
    iterator () -> Iterator<T>
}

pub trait Collection<T> {
    add    (v T)
    remove (v T)
    size   () -> int
}

-- struct: always valid
impl Circle : Drawable {
    draw   ()         = { ... }
    bounds () -> Rect = { ... }
}

-- primitive: valid — essential for generic algorithms
impl int : Comparable<int> {
    compareTo (other int) -> int = {
        if self > other { return 1 }
        if self < other { return -1 }
        return 0
    }
}

impl string : Hashable {
    hash   () -> uint64      = { ... }
    equals (other any) -> bool = { return self === other }
}

-- enum: valid
impl Direction : Comparable<Direction> {
    compareTo (other Direction) -> int = {
        return int(self) - int(other)
    }
}

impl ShaderStage : Hashable {
    hash   () -> uint64      = { return uint64(self) }
    equals (other any) -> bool = { return self === other }
}

-- type alias: valid — same underlying type, semantic context conformance
type ID = int

impl ID : Hashable {
    hash   () -> uint64      = { return uint64(self) }
    equals (other any) -> bool = { return self === other }
}

-- array type: valid — enables generic collection algorithms
impl [_, <T>] : Iterable<T> {
    iterator () -> Iterator<T> = { ... }
}

impl [*, <T>] : Collection<T> {
    add    (v T)     = { self:push(v) }
    remove (v T)     = { ... }
    size   () -> int = { return self:len() }
}

-- function type: FORBIDDEN
type PredicateFn = (int) -> bool
impl PredicateFn : Composable { ... }   -- ERROR: function types cannot implement traits
impl (int) -> bool : Composable { ... } -- ERROR: function types cannot implement traits
```

### Trait as a Type

When a trait name appears as a type — in a function parameter, return type, struct field, or variable declaration — the compiler enforces that every value provided at that position is a type that has declared conformance to the trait via `impl TypeName : TraitName`. The trait acts as a structural contract checked at each use site:

```luc
-- trait as parameter type
let render (shape Drawable) = {
    shape:draw()
    let r Rect = shape:bounds()
}

-- trait as return type
let createShape (kind string) -> Drawable = {
    if kind == "circle" { return Circle { radius = 1.0  center = Vec2 { x=0.0 y=0.0 } } }
    return Square { side = 1.0  origin = Vec2 { x=0.0 y=0.0 } }
}

-- trait as generic constraint
let drawAll<T : Drawable> (items [_, T]) = {
    for item T in items { item:draw() }
}

-- trait as struct field type
struct Scene {
    objects [_, Drawable]
}

-- call sites
render(Circle { radius = 5.0  center = Vec2 { x=0.0 y=0.0 } })  -- OK: Circle : Drawable
render(42)                                                         -- ERROR: int does not implement Drawable

drawAll<Circle>([...])    -- OK: Circle : Drawable
drawAll<int>([...])       -- ERROR: int has no Drawable impl
```

> [!NOTE]
> A trait name used as a type does not mean the value is stored as a trait object with dynamic dispatch. Luc resolves the concrete type at each call site at compile time. The trait name is a constraint — the compiler checks conformance and uses the concrete type's methods directly. There is no virtual dispatch table unless the language explicitly introduces one in a future version.

---

## Impl Declaration

`impl` can be declared at top level or inside a block. Local declarations **must not** have `pub` or `export`.

```
impl_decl       := [ visibility_mod ] 'impl' impl_target
                   [ impl_generic_params ]
                   [ 'as' IDENTIFIER ]
                   [ ':' trait_ref ]
                   '{' { method_decl } '}'

impl_target     := IDENTIFIER          -- named type (struct, enum, alias, primitive)
                 | primitive_type      -- primitive directly
                 | array_type          -- [_, T], [*, T], [N, T]  — concrete element type
                 | generic_array_type  -- [_, <T>], [*, <T>], [N, <T>]  — free type variable
                   -- generic_array_type is valid as an impl or from target
                   -- the '<T>' declares T as a free type variable unified at each call site
                   -- equivalent to: type Slice<T> = [_, T]  then  impl Slice<T>
                   -- T is then usable in method signatures as a plain IDENTIFIER

impl_generic_params := '<' impl_generic_param { ',' impl_generic_param } '>'

impl_generic_param  := IDENTIFIER [ ':' constraint_list ]

trait_ref       := IDENTIFIER [ generic_args ]

method_decl     := IDENTIFIER [ qualifier_list ] param_group { param_group }
                   [ '->' return_list ] '=' func_body -- inline body
                   -- qualifier_list applies to the method: ~async, ~nullable, ~parallel
                   -- type annotation required on all inline body methods

                 | IDENTIFIER '=' func_ref            -- plain assignment, no injection
                                                      -- full type INCLUDING qualifiers
                                                      -- is read from func_ref — no annotation
                                                      -- and no qualifier written on the method name

                 | IDENTIFIER '=' func_ref '(' receiver_arg ')' '!'
                   -- injection form
                   -- full type INCLUDING qualifiers is read from func_ref — no annotation
                   -- and no qualifier written on the method name
                   -- receiver_arg: must be 'self' or the impl's 'as' alias — nothing else
                   -- semantic phase removes the first parameter of the first group
                   -- and produces a new resolved function type
                   -- compiler generates an internal wrapper capturing the receiver
                   -- variadic @extern functions are forbidden with '!'
                   -- func_ref must be a named function reference — NOT a call expression,
                   -- NOT a returned function, NOT a partial application result

func_ref        := see Shared Productions in Notation section
                   -- local name, module path, or generic instantiation
                   -- qualifiers flow through from the referenced function
                   -- call expressions and factory functions are forbidden
```

The `as IDENTIFIER` clause introduces a local alias for the receiver inside the method bodies. If omitted, the receiver is accessible as `self`.

| `as` clause | Receiver name inside methods |
| ----------- | ---------------------------- |
| omitted     | `self`                       |
| `as alias`  | `alias`                      |

> [!WARNING]
> `as` and `self` are mutually exclusive
> If `as alias` is declared on the `impl` block, `self` is **undefined** inside every method body in that block. If `as` is omitted, only `self` is valid. The choice is made once at the `impl` header and applies to all methods uniformly — you cannot mix both names within the same `impl` block.
>
> ```luc
> impl int as number {
>     isEven () -> bool = { return number % 2 == 0 }   -- OK
>     isOdd  () -> bool = { return self % 2 != 0 }     -- ERROR: self undefined, use number
> }
>
> impl int {
>     isEven () -> bool = { return self % 2 == 0 }     -- OK
>     isOdd  () -> bool = { return number % 2 != 0 }   -- ERROR: number undefined, use self
> }
> ```

### Injection Form — `!` Semantics

When `= func_ref(receiver)!` is used, the semantic phase resolves the referenced function into a new function type by removing the first parameter of the first parameter group. The compiler generates an internal wrapper capturing the receiver. The resolved type is what the call site sees.

| Original signature                          | After `!` injection | Resolved call site type           |
| ------------------------------------------- | ------------------- | --------------------------------- |
| `(n int) -> string`                         | `n` fixed           | `() -> string`                    |
| `(n int, s string) -> string`               | `n` fixed           | `(s string) -> string`            |
| `(n int, s string, x int) -> string`        | `n` fixed           | `(s string, x int) -> string`     |
| `(n int)(lo int)(hi int) -> int`            | `n` fixed           | `(lo int)(hi int) -> int`         |
| `(s string, extra string)(n int) -> string` | `s` fixed           | `(extra string)(n int) -> string` |

The wrapper is a thin closure capturing the receiver. For primitive types and small structs the wrapper is inlined by the compiler — zero overhead in the final binary.

> [!WARNING]
> Injection restrictions
> - `func_ref` must be a **named function reference** — a named function, a module path, or a generic instantiation. It cannot be a function call, a factory function whose return value is another function, a partial application result, or any expression that produces a function value at runtime.
> - `func_ref` must **not** be a variadic `@extern` function (`args ...any`).
> - `receiver_arg` must be exactly `self` or the `as` alias declared on this `impl` block — no other value, no field access, no computed expression.
> - The first parameter group must have **at least one parameter** — the first parameter is the one removed.
> - The method's full type — **including any qualifiers** — is read from `func_ref`. No type annotation and no qualifier is written on the method name. This is the explicit exception to Luc's type annotation requirement.
>
> ```luc
> export const httpGet   ~async    (url string) -> string = { ... }
> export const maybeFind ~nullable (pred (int) -> bool) -> int = { ... }
> export const intToStr             (n int) -> string = { ... }
>
> impl string as s {
>     fetch  = httpGet(s)!       -- method type: ~async () -> string
>     toStr  = intToStr(s)!      -- method type: () -> string
> }
>
> impl [_, int] as list {
>     find   = maybeFind(list)!  -- method type: ~nullable (pred (int) -> bool) -> int
> }
>
> -- call sites — qualifier comes from the inferred method type
> let result string = await url:fetch()       -- ~async: must await
> let s      string = 42:toStr()             -- plain: no await
> if list:find != nil {                       -- ~nullable: must guard
>     list:find((v int) -> bool { return v > 2 })
> }
>
> impl int as i {
>     -- ERROR: function call as func_ref
>     addBase = makeAdder(10)(i)!
>
>     -- ERROR: field access as receiver_arg
>     xScale  = scaleFloat(i.x)!
>
>     -- ERROR: variadic @extern
>     printf  = printf(i)!
>
>     -- CORRECT: inline body for factory or computed receiver cases
>     addBase  (v int) -> int   = { return makeAdder(10)(v) + i }
>     xScale   (f float) -> float = { return scaleFloat(i.x, f) }
> }
> ```

### Naming Conflict Resolution

Within a single `impl` block, each method name must be unique — duplicate names in the same block are a compile error. Across multiple `impl` blocks for the same type, duplicate names are allowed. The compiler resolves them by **scope proximity** — the innermost or last-declared `impl` wins:

```luc
impl int {
    toStr () -> string = { return "first" }
}

impl int {
    toStr () -> string = { return "second" }    -- shadows the first
}

5:toStr()    -- "second"
```

For multiple imported files, the last `use` declaration wins:

```luc
use file1    -- file1 defines impl int { toStr }
use file2    -- file2 also defines impl int { toStr }

5:toStr()    -- uses file2's toStr — last import wins
```

When fine-grained control is needed — taking some methods from `file1` and some from `file2` — create a resolution file that explicitly declares a new `impl` block choosing each method:

```luc
-- ResolveImport.luc
use file1
use file2

impl int as i {
    toStr   = file1.intToStr(i)!     -- explicitly choose file1's version
    parse   = file2.parseInt(i)!     -- explicitly choose file2's version
    version = file1.getVersion       -- plain assignment, no injection
}
```

This `impl` block, being declared last, takes priority over both `file1` and `file2` for the methods it declares. Methods not mentioned here fall through to the normal last-import-wins resolution.

### Impl Target Rules

| Target type                                              | Allowed directly? | Example                                         |
| -------------------------------------------------------- | ----------------- | ----------------------------------------------- |
| **Primitive** (`int`, `float`, `string`, `bool`, `char`) | ✅ Yes             | `impl int { isEven () -> bool = { … } }`        |
| **Struct**                                               | ✅ Yes             | `impl Vec2 { length () -> float = { … } }`      |
| **Enum**                                                 | ✅ Yes             | `impl Direction { isNorth () -> bool = { … } }` |
| **Type alias**                                           | ✅ Yes             | `impl IntList { sum () -> int = { … } }`        |
| **Array type** (`[_, T]`, `[*, T]`, `[N, T]`)            | ✅ Yes             | `impl [_, int] { sum () -> int = { … } }`       |
| **Trait**                                                | ❌ No              | Traits are contracts, not implementations       |

> [!NOTE]
> Trait conformance (`impl TypeName : TraitName`) is valid for all `impl` targets **except function types**. Structs, primitives, enums, type aliases, and array types can all implement traits. See the Trait section for details and examples.

### Type Alias Transparency

A type alias is just another name for the underlying type — it does not create a new distinct type. This means `impl int` and `impl Numeric` (where `type Numeric = int`) target the **same type** and their methods are merged by the same scope-proximity rule that resolves any other `impl` conflict.

```luc
type Numeric = int
type ID      = int

impl int as i {
    isEven () -> bool = { return i % 2 == 0 }
}

impl Numeric as n {
    isPositive () -> bool = { return n > 0 }    -- same target as impl int
}

impl ID as id {
    isZero () -> bool = { return id == 0 }      -- also same target as impl int
}

-- all three methods are available on any int value
let x int = 5
x:isEven()       -- OK: from impl int
x:isPositive()   -- OK: from impl Numeric — same target
x:isZero()       -- OK: from impl ID — same target
```

This also holds for struct aliases — `impl Vec2` and `impl Point` (where `type Point = Vec2`) target the same struct:

```luc
struct Vec2 { x float  y float }
type Point = Vec2

impl Vec2 as v {
    length () -> float = { return #sqrt(v.x*v.x + v.y*v.y) }
}

impl Point as p {
    distanceTo (other Point) -> float = {
        let dx float = p.x - other.x
        let dy float = p.y - other.y
        return #sqrt(dx*dx + dy*dy)
    }
}

let a Vec2  = Vec2  { x = 3.0  y = 4.0 }
let b Point = Point { x = 0.0  y = 0.0 }

a:length()          -- OK: from impl Vec2
a:distanceTo(b)     -- OK: from impl Point — same target
b:length()          -- OK: Point is Vec2, both methods available
```

For arrays, `impl [_, int]` and `impl IntList` (where `type IntList = [_, int]`) target the same array type:

```luc
type IntList = [_, int]

impl [_, int] as s {
    sum () -> int = {
        let total int = 0
        for v int in s { total += v }
        return total
    }
}

impl IntList as s {
    product () -> int = {
        let result int = 1
        for v int in s { result *= v }
        return result
    }
}

let nums [_, int] = [1, 2, 3, 4, 5]
nums:sum()        -- 15  — from impl [_, int]
nums:product()    -- 120 — from impl IntList, same target

-- also callable via an IntList variable
let list IntList = [2, 3, 4]
list:sum()        -- 9
list:product()    -- 24
```

For generic arrays, `impl [_, <T>]` and `impl Slice<T>` (where `type Slice<T> = [_, T]`) are equivalent:

```luc
type Slice<T> = [_, T]

impl [_, <T>] as s {
    first () -> T = { return s[0] }
}

impl Slice<T> as s {
    last () -> T = { return s[s:len() - 1] }
}

let nums [_, int] = [1, 2, 3]
nums:first()    -- 1  — from impl [_, <T>] with T = int
nums:last()     -- 3  — from impl Slice<T> with T = int, same target
```

> [!NOTE]
> Because type aliases are transparent, naming conflicts between `impl int` and `impl Numeric` are resolved by scope proximity — the innermost or last-declared block wins, exactly as with any two `impl` blocks for the same type. The alias name used in the `impl` header has no effect on resolution — only the underlying target type matters.

> [!WARNING]
> `impl` on array types
>
> Methods defined on `[_, int]` apply to every slice of `int` in the visible scope. Use with care or prefer a named alias for clarity and narrower scope. There are no built-in array methods to shadow — the compiler only knows indexing and slicing.
>
> Arrays of function types are valid targets — the element function type must be an exact signature match including qualifiers:
>
> ```luc
> impl [_, (int) -> bool] as predicates {
>     applyAll (value int) -> [_, bool] = {
>         let results [*, bool] = []
>         for f (int) -> bool in predicates { results:push(f(value)) }
>         return results
>     }
> }
> ```

### Generic Array `impl`

When a method needs to refer to the element type — for example returning `T` from `first()` or accepting `T` in `push()` — use the `<T>` syntax inside the bracket to declare a free type variable:

```luc
impl [_, <T>] as s {
    first () -> T = { return s[0] }
    last  () -> T = { return s[s:len() - 1] }
}

impl [*, <T>] as a {
    push  (v T)   = { _dynarrayPush(#refToPtr(a), #refToPtr(v)) }
    pop   () -> T = { return #ptrToRef(_dynarrayPop(#refToPtr(a))) }
    first () -> T = { return a[0] }
    last  () -> T = { return a[a:len() - 1] }
}
```

The `<T>` in the target declares `T` as a free type variable. Inside the method signatures and bodies, `T` is a plain identifier referring to the declared variable. The compiler unifies `T` with the concrete element type at each call site — no instantiation needed.

**The two forms are equivalent.** The compiler treats them identically:

```luc
-- form 1: inline generic array target
impl [_, <T>] as s {
    first () -> T = { return s[0] }
}

-- form 2: via type alias
type Slice<T> = [_, T]
impl Slice<T> as s {
    first () -> T = { return s[0] }
}
```

Use the inline form for brevity. Use the alias form when the type already has a meaningful name or is reused elsewhere.

**Precedence — most specific target wins:**

```luc
impl [_, int] as s {
    sum () -> int = { ... }         -- most specific: wins for [_, int]
}

impl [_, <T>] as s {
    sum () -> T = { ... }           -- structural: wins for all other [_, T]
}

impl [_, any] as s {
    sum () -> any = { ... }         -- concrete any: wins only for [_, any]
}
```

For a `[_, int]` array, `impl [_, int]` wins. For a `[_, string]` array, `impl [_, <T>]` wins with `T = string`. For a `[_, any]` array, `impl [_, any]` wins.

**`any` is not a type variable:**

`impl [_, any]` is a concrete impl that applies only to arrays whose declared element type is literally `any`. It is not equivalent to `impl [_, <T>]`:

```luc
let nums  [_, int]  = [1, 2, 3]
let items [_, any]  = [1, "hello", true]

nums:sum()     -- resolves to impl [_, int]  or impl [_, <T>] with T=int
items:sum()    -- resolves to impl [_, any]  only
```

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
| Trait                    | ❌ No                  | N/A                     | Traits are contracts, not implementations                 |

### Dependent vs Independent Generic Parameters in Methods

`impl` methods may freely use both dependent and independent type parameters. The distinction determines how the call site provides them:

**Dependent parameters** — come from the `impl` target declaration (e.g. `T` in `impl Foo<T>`). They are already resolved when the instance is declared. The compiler infers them automatically at the call site. Writing them explicitly is a compile error:

```luc
impl Foo<T> as f {
    doSomething (v T) -> T = { return v }
}

let a Foo<int> = Foo<int> { value = 42 }

a:doSomething(42)           -- OK: T = int inferred from Foo<int>
a:doSomething<int>(42)      -- ERROR: T is dependent, do not write explicitly
a:doSomething<string>(42)   -- ERROR: T is dependent, contradicts Foo<int>
```

**Independent parameters** — declared on the method itself (e.g. `<U>` in `convert<U>`). They are resolved fresh at each call site. The caller must provide them explicitly. Omitting them is a compile error:

```luc
impl Foo<T> as f {
    convert<U> (fn (T) -> U) -> U = { return fn(f.value) }
}

let a Foo<int> = Foo<int> { value = 42 }

a:convert<string>(toString<int>)    -- OK: U = string, explicit required
a:convert(toString<int>)            -- ERROR: U is independent, must provide
```

**Mixing dependent and concrete in method assignments:**

In the injection form, `func_ref<...>` may mix dependent parameters with concrete type arguments:

```luc
export const magic<A, B> (v A, fn (A) -> B) -> B = { return fn(v) }

impl Foo<T> as f {
    toInt    = magic<T, int>(f)!     -- T dependent, int concrete
                                     -- resolved: (fn (T) -> int) -> int
    convert<U> = magic<T, U>(f)!    -- T dependent, U independent
                                     -- resolved: (fn (T) -> U) -> U
}

let a Foo<string> = Foo<string> { value = "hello" }
a:toInt(str:len)                  -- OK: T=string inferred, int fixed
a:convert<bool>(str:isEmpty)      -- OK: T=string inferred, U=bool explicit
```

**Why `impl` allows independent generics but structs do not:**

`impl` only contains methods — function pointers. A function pointer has a fixed size (one pointer width) regardless of its type parameters. Independent `<U>` is resolved at the call site, not at the impl declaration. No memory layout is affected.

Struct fields contain stored values. Every field must have a known size at instantiation time. An independent `<U>` with no resolution site produces unknown size — the compiler cannot lay out the struct in memory.

### Examples

```luc
-- No alias — receiver accessed as self
impl Vec2 {
    length () -> float = { return #sqrt(self.x*self.x + self.y*self.y) }
}

-- With alias — receiver accessed as v
impl Vec2 as v {
    length () -> float = { return #sqrt(v.x*v.x + v.y*v.y) }
}

-- Plain assignment — no injection, function exposed as-is
impl int as i {
    version = utils.getVersion     -- () -> string, called as 5:version()
}

-- Injection form — receiver fixed, resolved type at call site
impl int as i {
    toStr     = utils.intToStr(i)! -- (n int) -> string         resolved: () -> string
    format    = utils.format(i)!   -- (n int, s string) -> str  resolved: (s string) -> string
    clamp     = utils.clamp(i)!    -- (n int)(lo int)(hi int)   resolved: (lo int)(hi int) -> int
    process   = utils.process(i)!  -- (n int, s string)(x int)  resolved: (s string)(x int) -> int
}

-- call sites match the resolved types exactly
5:toStr()              -- () -> string
5:format("prefix")     -- (s string) -> string
5:clamp(0)(100)        -- (lo int)(hi int) -> int
5:process("a")(3)      -- (s string)(x int) -> int
5:version()            -- () -> string

-- Generic instantiation with injection
impl int as i {
    toStr = utils.toStr<int>(i)!    -- generic instantiated, i injected
}

-- Generic function assigned to generic struct impl
-- The generic parameter T flows from impl Box<T> into the method assignment
export const unwrap<T>  (box Box<T>) -> T      = { return box.value }
export const rewrap<T>  (box Box<T>, v T) -> Box<T> = { return Box<T> { value = v } }
export const transform<T, U> (box Box<T>, f (T) -> U) -> Box<U> = {
    return Box<U> { value = f(box.value) }
}

impl Box<T> as b {
    -- inject b as first param, T flows through from impl Box<T>
    unwrap    = unwrap<T>(b)!
    rewrap    = rewrap<T>(b)!

    -- transform has two param groups: (box Box<T>)(f ...) → after injection: (f (T) -> U) -> Box<U>
    -- U is a free type variable resolved at the call site
    map<U>    = transform<T, U>(b)!    -- generic method: extra type param U at call site
}

-- call sites
let box  Box<int>    = Box<int> { value = 42 }
box:unwrap()                              -- 42
box:rewrap(100)                           -- Box<int> { value = 100 }
box:map<string>(toString<int>)            -- Box<string> { value = "42" }

-- Generic array impl with generic function injection
export const sortSlice<T : Comparable> (s [_, T]) -> [_, T] = { ... }
export const findFirst<T> (s [_, T], pred (T) -> bool) -> T? = { ... }

impl [_, <T>] as s {
    -- plain generic injection: T flows from [_, <T>]
    sort      = sortSlice<T>(s)!

    -- findFirst needs a pred argument at the call site
    findFirst = findFirst<T>(s)!    -- resolved: (pred (T) -> bool) -> T?
}

let nums [_, int] = [3, 1, 4, 1, 5]
nums:sort()                                       -- sorted slice
nums:findFirst((v int) -> bool { return v > 3 }) -- 4?

-- @extern single param — works perfectly
@extern("strlen")
const strlen (s *uint8) -> uint64

impl [_, byte] as buf {
    len = strlen(buf)!    -- resolved: () -> uint64
}

-- @extern multiple params — works, remaining params at call site
@extern("memset")
const memset (dst *uint8, val uint8, len uint64) -> *uint8

impl [_, byte] as buf {
    fill = memset(buf)!    -- resolved: (val uint8, len uint64) -> *uint8
}
myBuf:fill(0xFF, 1024)     -- memset(myBuf, 0xFF, 1024)

-- @extern variadic — forbidden with !, use inline body
@extern("printf", "C")
const printf (fmt *uint8, args ...any) -> int

impl string as s {
    -- print = printf(s)!    -- ERROR: variadic @extern forbidden with !
    print (args ...any) -> int = { return printf(s, args) }    -- inline body instead
}

-- Mixed forms in one block
impl int as i {
    isEven   () -> bool    = { return i % 2 == 0 }  -- inline body
    toStr    = intToStr(i)!                         -- injection
    version  = getVersion                           -- plain assignment
}

-- With generics and alias
impl Box<T> as b {
    get () -> T = { return b.value }
}

-- With alias and trait conformance
impl Circle as c : Drawable {
    draw () = { c:render() }
}

-- Array type with alias and injection
impl [_, int] as list {
    sum  () -> int = {
        let total int = 0
        for v int in list { total += v }
        return total
    }
    sort = utils.sortSlice(list)!    -- resolved: () -> [_, int]
}

-- Generic type alias
type Pair<T, E> = struct { ok T?, err E? }

impl Pair<T, E> as p {
    isOk () -> bool = { return p.ok != nil }
}

-- ERROR cases
impl int<T> { ... }         -- ERROR: primitive cannot have generics
impl Enum<T> { ... }        -- ERROR: enum cannot have generics
impl Vec2<T> { ... }        -- ERROR: non-generic struct cannot have generics
impl Pair<X> { ... }        -- ERROR: arity mismatch (needs 2 parameters, got 1)
```

---

## From Declaration

`from` can be declared at top level or inside a block. Local declarations **must not** have `pub` or `export`.

```
from_decl   := [ visibility_mod ] 'from' from_target [ generic_params ]
               '{' { from_entry } '}'

from_target := type                -- any named, primitive, array, or alias type
             | generic_array_type  -- [_, <T>], [*, <T>], [N, <T>]  — free type variable
                                   -- same rule as impl: T declared here, usable in entries

from_entry  := param_group { param_group } '->' type '=' func_body   -- inline entry
             | func_ref                                               -- path entry
               -- compiler reads func_ref's full signature INCLUDING qualifiers
               -- as the entry signature — no type annotation written
               -- the return type must match the enclosing from target type
               -- no receiver injection: from entries convert TO the target, not on it

func_ref    := see Shared Productions in Notation section
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

**Inline entry** — explicitly declares the source signature and body:

- One or more parameter groups (currying allowed) — the source value(s).
- The arrow `->` (mandatory).
- A single return type — must match the enclosing `from` target type.
- `=` followed by the conversion body (a block or expression).
- No qualifiers (`~async`, `~nullable`, `~parallel`) — `from` entries are plain functions.

**Path entry** — references an existing function by name:

- The compiler reads the function's full signature — including any qualifiers — and registers it as a `from` entry. No type annotation is written.
- The function's return type must match the enclosing `from` target type — compile error otherwise.
- No receiver injection (`!`) — `from` converts *to* the target type, not *on* it.
- Local functions, imported functions, and `@extern` non-variadic functions are all valid.
- Function calls, expressions that produce functions, and factory functions are forbidden — use an inline entry for those cases.

**Generic function as path entry — concrete target:**

A generic function can be used as a path entry by instantiating it at the entry site with explicit type arguments. The compiler reads the instantiated signature and registers it as a conversion entry. The target type must match the instantiated return type:

```luc
export const toString<T>  (v T)      -> string = { ... }
export const parseAs<T>   (s string) -> T      = { ... }

-- concrete target: instantiate at entry site
from string {
    toString<int>      -- registers (int) -> string
    toString<float>    -- registers (float) -> string
    toString<bool>     -- registers (bool) -> string
}

from int {
    parseAs<int>       -- registers (string) -> int
}
```

**Generic function as path entry — generic target:**

When the `from` target is a generic type, its type parameters are available to path entries — the same rule as `impl Box<T>`. The generic parameter flows from the `from` declaration into the path entry instantiation:

```luc
struct Box<T> { value T }

export const wrap<T>   (v T)      -> Box<T> = { return Box<T> { value = v } }
export const rewrap<T> (s string) -> Box<T> = { return Box<T> { value = parseAs<T>(s) } }

-- T flows from from Box<T> into the path entries
from Box<T> {
    wrap<T>      -- registers (T) -> Box<T>
    rewrap<T>    -- registers (string) -> Box<T>
}

-- call sites
let b  Box<int>    = Box<int>(42)       -- uses wrap<int>
let b2 Box<int>    = Box<int>("42")     -- uses rewrap<int>
let b3 Box<string> = Box<string>("hi")  -- uses wrap<string>
```

**Independent generics are forbidden in `from` entries:**

`from` conversions are implicit — there is no syntax at the conversion site to provide an independent type argument. Every type parameter used in a `from` entry must come from the `from` target declaration. An independent `<U>` with no conversion-site slot is a compile error:

```luc
export const rewrap<T, U> (v T, fn (T) -> U) -> Box<U> = { ... }

from Box<T> {
    wrap<T>          -- OK: T from from Box<T>
    rewrap<T, U>     -- ERROR: U is independent — no syntax to provide U at conversion site
}

-- CORRECT: fix U concretely at the from declaration
from Box<string> {
    rewrap<int, string>    -- OK: both T and U concrete
}
```

**Conflict resolution** — if two `from` entries in the same block have identical source signatures, it is a compile error. Across multiple `from` blocks for the same target type, the innermost or last-declared block wins by the same scope-proximity rule as `impl`.

The compiler does not chain conversions (e.g., A → B → C) — only a single direct conversion is applied.

> [!NOTE]
> `from` on array and function types
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
-- Convert from string to int (inline entry)
from int {
    (s string) -> int = {
        return #parseInt(s)
    }
}

-- Convert from Celsius and Kelvin to Fahrenheit (inline entries)
export from Fahrenheit {
    (c Celsius) -> Fahrenheit = {
        return Fahrenheit { value = c.value * 9.0 / 5.0 + 32.0 }
    }
    (k Kelvin) -> Fahrenheit = {
        return Fahrenheit { value = (k.value - 273.15) * 9.0 / 5.0 + 32.0 }
    }
}

-- Convert from string to Direction (inline entry)
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

-- Path entries — compiler reads each function's signature
-- utils.floatToStr : (f float) -> string  → registered as from entry
-- utils.doubleToStr: (d double) -> string → registered as from entry
from string {
    (n int) -> string { return string(n) } -- inline entry
    utils.floatToStr                       -- path entry: (float) -> string
    utils.doubleToStr                      -- path entry: (double) -> string
    localCharToStr                         -- path entry: local function
}

-- Path entries with generic instantiation — concrete target
-- instantiate at the entry site, compiler reads the resolved signature
export const toString<T>  (v T)      -> string = { ... }
export const parseAs<T>   (s string) -> T      = { ... }

from string {
    toString<int>      -- registers (int)    -> string
    toString<float>    -- registers (float)  -> string
    toString<bool>     -- registers (bool)   -> string
}

from int {
    parseAs<int>       -- registers (string) -> int
}

let s1 string = string(42)       -- uses toString<int>
let s2 string = string(3.14)     -- uses toString<float>
let n  int    = int("42")        -- uses parseAs<int>

-- Generic function path entry — generic target
-- T flows from from Box<T> into path entries, same rule as impl Box<T>
struct Box<T> { value T }

export const wrap<T>   (v T)      -> Box<T> = { return Box<T> { value = v } }
export const rewrap<T> (s string) -> Box<T> = { return Box<T> { value = parseAs<T>(s) } }

from Box<T> {
    wrap<T>      -- registers (T)      -> Box<T>
    rewrap<T>    -- registers (string) -> Box<T>
}

let b1 Box<int>    = Box<int>(42)      -- uses wrap<int>
let b2 Box<int>    = Box<int>("42")    -- uses rewrap<int>
let b3 Box<string> = Box<string>("hi") -- uses wrap<string>

-- Generic array target
export const sliceOf<T> (v T) -> [_, T] = { return [v] }

from [_, <T>] {
    sliceOf<T>    -- registers (T) -> [_, T]
}

let nums [_, int]    = [_, int](42)      -- [42]
let strs [_, string] = [_, string]("hi") -- ["hi"]

-- From on array type directly
from [_, int] {
    (r Range) -> [_, int] = {
        let result [*, int] = []
        for i int in r { result:push(i) }
        return result
    }
    utils.rangeToSlice    -- path entry: (Range) -> [_, int]
}

-- From on function type
from (int) -> bool {
    -- inline: strip ~nullable qualifier with a fallback
    (f ~nullable (int) -> bool) -> (int) -> bool = {
        if f == nil { return (x int) -> bool { return false } }
        return f
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
    from int {
        (s string) -> int = { return #parseInt(s) }
    }
    let x int = "42"  -- uses the local from declaration
    return x
}

-- Conflict resolution — two from blocks for same target, last wins
from int {
    utils.parseInt    -- (string) -> int from utils
}

from int {
    myLib.parseInt    -- (string) -> int from myLib — shadows utils version
}

-- ResolveImport pattern — explicit control over which version wins
use file1    -- file1.floatToInt: (float) -> int
use file2    -- file2.floatToInt: (float) -> int

from int {
    file1.floatToInt    -- explicitly choose file1's version
    file2.doubleToInt   -- take double conversion from file2
}

-- ERROR cases
from int {
    utils.badReturn        -- ERROR: return type is string, not int
    utils.printf           -- ERROR: printf is variadic @extern
    parseIntFromStr()      -- ERROR: function call, not a reference
}
```

---

## Pipeline Operator `|>`

`|>` executes a chain left-to-right at runtime.

```
pipeline_expr   := pipeline_seed { '|>' pipeline_step }

pipeline_seed   := expr

pipeline_step   := func_ref                            -- named function, module path, or fn<T>
                 | expr ':' IDENTIFIER                 -- method call on value
                 | IDENTIFIER '.' IDENTIFIER           -- non-nullable data field
                 | func_ref '(' arg_list ')' '!'       -- argument pack
                 | anon_func
                   -- func_ref: see Shared Productions in Notation section
                   -- ~nullable and ~parallel func_ref forbidden as steps (see Rules)
```

| Step form      | What `\|>` does                           | Nullability                |
| -------------- | ----------------------------------------- | -------------------------- |
| `fn`           | calls `fn(upstream)`                      | must be non-nullable       |
| `var:method`   | calls `method(upstream)`                  | always safe                |
| `struct.field` | calls function stored in field            | field must be non-nullable |
| `fn(args)!`    | calls `fn(upstream, args...)`             | must be non-nullable       |
| `fn<T>`        | instantiates generic, calls with upstream | must be non-nullable       |
| `anon_func`    | calls with upstream as argument           | always safe                |

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

**Generic functions as pipeline steps:**

A generic function can be used as a pipeline step by instantiating it with explicit type arguments. This is consistent with the assignment form `let f = identity<int>` — the `<T>` produces a concrete function value that `|>` can call:

```luc
export const identity<T> (v T) -> T = { return v }
export const map<T, U>   (v T, f (T) -> U) -> U = { return f(v) }

let result int    = 42       |> identity<int>
let result string = 42       |> map<int, string>(string)!
let result int    = "hello"  |> map<string, int>(str:len)!
```

An uninstantiated generic function is not a valid pipeline step — type arguments are always required:

```luc
42 |> identity        -- ERROR: generic function without type arguments
42 |> identity<int>   -- OK
```

**Qualifier behavior in pipeline steps:**

- Steps with `~async` are allowed. The pipeline expression becomes `~async` (its type includes `~async`). The caller must `await` the entire pipeline result.
- Steps with `~nullable` are forbidden.
- Steps with `~parallel` are forbidden — pipeline execution is synchronous.

> [!WARNING]
> Curry functions are forbidden as pipeline steps
> A curry function has more than one parameter group. When used as a pipeline step, `|>` injects the upstream value into the **first** parameter group only. The remaining parameter groups are never filled — there is no upstream for them and no call syntax to provide them inline. This leaves the function partially applied with no way to complete the call, which is a compile error.
>
> ```luc
> export const clamp (lo int)(hi int)(v int) -> int = { ... }
> export const add   (a int)(b int) -> int          = { ... }
>
> 42 |> clamp        -- ERROR: clamp is a curry function — upstream fills (lo int),
>                    -- but (hi int) and (v int) have no values
> 42 |> add          -- ERROR: add is a curry function — upstream fills (a int),
>                    -- but (b int) has no value
>
> -- CORRECT: use argument pack to pre-fill all but the last group
> 42 |> clamp(0)(100)!   -- ERROR: ! is for single extra args, not multi-group curry
>
> -- CORRECT: instantiate to a single-group function first
> let clamp0to100 (v int) -> int = clamp(0)(100)
> 42 |> clamp0to100      -- OK: clamp0to100 has one parameter group
>
> -- CORRECT: use anonymous function to wrap
> 42 |> (v int) -> int { return clamp(0)(100)(v) }   -- OK
> ```
>
> This applies to all curry function kinds — concrete, generic (instantiated or not), and method references that resolve to curry signatures. If the step resolves to a function with more than one parameter group, it is a compile error.

> [!WARNING]
> Alias bypass is not allowed
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

compose_operand := func_ref                      -- named function, module path, or fn<T>
                 | expr ':' IDENTIFIER           -- method reference on a value
                 | expr '.' IDENTIFIER           -- non-nullable data field only
                   -- func_ref: see Shared Productions in Notation section
                   -- ~nullable func_ref forbidden as operand (see Rules)
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

### Generic Functions and `+>`

`+>` is where generic functions shine. A single generic function can act as a universal adapter between any two compatible types — eliminating the need for concrete one-off wrapper functions.

**Instantiate a generic at the composition site:**

```luc
export const toString<T>  (v T)      -> string = { ... }
export const parseFloat   (s string) -> float  = { ... }
export const double       (x float)  -> float  = { return x * 2.0 }

-- compose: int → string → float → float
let intToDoubled (x int) -> float = toString<int> +> parseFloat +> double

-- call — no boilerplate wrapper needed
let result float = intToDoubled(42)    -- "42" → 42.0 → 84.0
```

**Generic adapters — compose different kinds of pipelines:**

A generic function with matching input/output types can slot into any composition chain where the types align:

```luc
export const map<T, U>    (v T, f (T) -> U) -> U = { return f(v) }
export const filter<T>    (v T, pred (T) -> bool) -> T? = {
    if pred(v) { return v }
    return nil
}
export const withDefault<T> (v T?, fallback T) -> T = { return v ?? fallback }

-- compose a processing chain using generic adapters
let processScore (score int) -> string =
    map<int, string>(toString<int>)!       +>
    map<string, int>(parseIntFromStr)!     +>
    filter<int>(isPositive)!               +>
    withDefault<int>(0)!                   +>
    toString<int>
```

**Building reusable pipeline factories:**

Generic functions make it easy to build configurable pipelines as values:

```luc
export const clamp<T : Comparable> (lo T)(hi T)(v T) -> T = { ... }
export const scale<T : Numeric>    (factor T)(v T) -> T   = { ... }

-- concrete pipeline built from generic instantiations
let normalizeScore (score int) -> int =
    clamp<int>(0)(100) +>    -- ERROR: curry — pre-apply first
    scale<int>(2)            -- ERROR: curry — pre-apply first

-- CORRECT: pre-apply curry groups to produce single-group functions
let clamp0to100 (v int) -> int = clamp<int>(0)(100)
let scaleBy2    (v int) -> int = scale<int>(2)

let normalizeScore (score int) -> int = clamp0to100 +> scaleBy2

normalizeScore(150)    -- 100 → 200 (clamped first, then scaled)
normalizeScore(-10)    -- 0   → 0
normalizeScore(50)     -- 50  → 100
```

**Generic composition with qualifiers:**

Qualifiers live on the binding, not on the composed result. A chain of generic functions with different qualifiers requires explicit qualifier assignment:

```luc
export const fetch<T>  ~async (url string) -> T    = { ... }
export const parse<T>         (raw string) -> T    = { ... }
export const format<T>        (v T) -> string      = { ... }

-- composed pipeline is async because fetch is async
let fetchAndFormat ~async (url string) -> string =
    fetch<string> +> parse<string> +> format<string>

let result string = await fetchAndFormat("https://api.example.com/data")
```

**Why `+>` with generics reduces boilerplate:**

Without generics, every type combination needs a wrapper:

```luc
-- without generics: one wrapper per type combination
let intToStr    (v int)    -> string = { return string(v) }
let floatToStr  (v float)  -> string = { return string(v) }
let boolToStr   (v bool)   -> string = { return string(v) }

let pipeInt   = validate     +> intToStr   +> trim
let pipeFloat = validateFloat +> floatToStr +> trim
let pipeBool  = validateBool  +> boolToStr  +> trim
```

With generics, one function covers all:

```luc
-- with generics: one function, instantiate at composition site
export const toString<T> (v T) -> string = { return string(v) }

let pipeInt   = validateInt   +> toString<int>   +> trim
let pipeFloat = validateFloat +> toString<float> +> trim
let pipeBool  = validateBool  +> toString<bool>  +> trim
```

> [!NOTE]
> An uninstantiated generic function is not a valid `+>` operand — type arguments are always required. The compiler cannot infer types across `+>` boundaries:
>
> ```luc
> let h = f +> toString        -- ERROR: toString is generic, type args required
> let h = f +> toString<int>   -- OK
> ```

> [!WARNING]
> Curry functions are forbidden as `+>` operands for the same reason as in `|>` — `+>` wires the output of the left to the input of the right. A curry function with multiple parameter groups has no single input type to wire to. Pre-apply all curry groups to produce a single-group function before composing:
>
> ```luc
> let clamp0to100 (v int) -> int = clamp<int>(0)(100)   -- single group
> let process = validate +> clamp0to100 +> toString<int>  -- OK
> let bad     = validate +> clamp<int>                    -- ERROR: curry operand
> ```

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
                 | '?.' IDENTIFIER                -- nullable chain: access field on nullable value
                                                  -- must be terminated by '??' in the expression
                 | '??' expr                      -- lhs is nil or T!E: result is rhs
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

> [!WARNING]
> Restrictions on arrays of function types
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

> [!TIP]
> Arrays of function types enable
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

All array operations beyond indexing and slicing are provided by the standard library. The standard library exposes every operation in **two forms simultaneously** — a standalone function and an `impl` method. The developer chooses whichever fits their style.

#### How the Standard Library Is Structured

The standard library uses `@extern` to call into the Luc runtime for operations that require access to the internal array header (length, capacity, allocation). It then wraps those into exported functions, and registers those functions as `impl` methods via the injection form:

```luc
-- ── @extern runtime calls (internal, not exported) ─────────────────

@extern("luc_slice_len")
const _sliceLen     (arr *uint8) -> uint64

@extern("luc_dynarray_push")
const _dynarrayPush (arr *uint8, elem *uint8)

@extern("luc_dynarray_pop")
const _dynarrayPop  (arr *uint8) -> *uint8

@extern("luc_dynarray_clear")
const _dynarrayClear (arr *uint8)

@extern("luc_dynarray_reserve")
const _dynarrayReserve (arr *uint8, n uint64)


-- ── exported functions: usable directly ─────────────────────────────

export const len<T>     (arr [_, T])          -> uint64 = { return _sliceLen(#toPtr(arr)) }
export const len<T>     (arr [*, T])          -> uint64 = { return _sliceLen(#toPtr(arr)) }
export const push<T>    (arr [*, T], v T)               = { _dynarrayPush(#toPtr(arr), #toPtr(v)) }
export const pop<T>     (arr [*, T])          -> T      = { return #toRef(_dynarrayPop(#toPtr(arr))) }
export const clear<T>   (arr [*, T])                    = { _dynarrayClear(#toPtr(arr)) }
export const reserve<T> (arr [*, T], n uint64)          = { _dynarrayReserve(#toPtr(arr), n) }
export const first<T>   (arr [_, T])          -> T      = { return arr[0] }
export const last<T>    (arr [_, T])          -> T      = { return arr[arr:len() - 1] }


-- ── impl blocks: usable as methods via injection ─────────────────────

impl [_, <T>] as s {
    len     = std.len<T>(s)!
    isEmpty () -> bool = { return s:len() == 0 }
    first   = std.first<T>(s)!
    last    = std.last<T>(s)!
}

impl [*, <T>] as a {
    len     = std.len<T>(a)!
    isEmpty () -> bool = { return a:len() == 0 }
    push    = std.push<T>(a)!
    pop     = std.pop<T>(a)!
    clear   = std.clear<T>(a)!
    reserve = std.reserve<T>(a)!
    first   = std.first<T>(a)!
    last    = std.last<T>(a)!
}

impl [N, <T>] as b {
    len     () -> uint64 = { return N }    -- compile-time constant, no extern needed
    isEmpty () -> bool   = { return N == 0 }
    first   = std.first<T>(b)!
    last    = std.last<T>(b)!
}
```

#### Two Options for the Developer

**Option 1 — method style via `impl`:**

```luc
let nums [*, int] = [1, 2, 3, 4, 5]

nums:push(6)
let n     = nums:len()
let first = nums:first()
nums:clear()
```

**Option 2 — function style directly:**

```luc
let nums [*, int] = [1, 2, 3, 4, 5]

std.push(nums, 6)
let n     = std.len(nums)
let first = std.first(nums)
std.clear(nums)
```

**Option 2 also works in pipelines:**

```luc
let n = nums |> std.len
```

Both options are zero-overhead — the `impl` injection wrapper is inlined by the compiler to the same direct call as the function style.

#### What Requires `@extern`

Operations that require touching the internal array header cannot be expressed in plain Luc code and must use `@extern`:

| Operation    | Requires `@extern`? | Why                            |
| ------------ | ------------------- | ------------------------------ |
| `len()`      | ✅ Yes               | reads internal header field    |
| `cap()`      | ✅ Yes               | reads internal header field    |
| `push()`     | ✅ Yes               | mutates header, may reallocate |
| `pop()`      | ✅ Yes               | mutates header                 |
| `clear()`    | ✅ Yes               | frees buffer, resets header    |
| `reserve()`  | ✅ Yes               | allocates heap buffer          |
| `first()`    | ❌ No                | only uses indexing             |
| `last()`     | ❌ No                | only uses indexing + `len()`   |
| `sum()`      | ❌ No                | only uses iteration            |
| `contains()` | ❌ No                | only uses iteration            |

#### Writing Your Own Array Methods

Developers can add methods to any array type using `impl`. Operations that only need indexing and iteration require no `@extern`. Use `[_, <T>]` when the method needs to reference the element type:

```luc
-- element-type-aware method using <T>
impl [_, <T>] as list {
    contains (target T) -> bool = {
        for v T in list {
            if v == target { return true }
        }
        return false
    }
}

-- concrete element type — no <T> needed
impl [_, int] as list {
    sum () -> int = {
        let total int = 0
        for v int in list { total += v }
        return total
    }
}

-- use as method or standalone function
let nums [_, int] = [1, 2, 3, 4, 5]
nums:sum()            -- 15
nums:contains(3)      -- true
```

Operations that need the internal header must go through `@extern`:

```luc
-- declare the runtime function
@extern("luc_slice_len")
const _sliceLen (arr *uint8) -> uint64

-- wrap it
export const myLen<T> (arr [_, T]) -> uint64 = {
    return _sliceLen(#toPtr(arr))
}

-- register as method
impl [_, int] as s {
    myLen = myLen<int>(s)!
}
```

> [!NOTE]
> The standard library's `impl` blocks follow the same scope-proximity rules as any other `impl` — a local `impl` in a narrower scope wins. Developers can selectively override individual methods without replacing the entire standard implementation.

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

`else` is optional. The compiler applies **type narrowing** inside branches based on the condition.

#### Standard Narrowing — Inside the Block

When the condition checks a nullable variable, the compiler narrows its type inside the then-branch:

```luc
let a int? = getValue()

if a != nil {
    -- a is int here, not int?
    let x int = a + 1    -- OK
}
-- a is still int? here
```

The `is` expression also narrows inside the then-branch:

```luc
let x any = getValue()

if x is int {
    -- x is int here
}
```

#### Inverse Narrowing — Early Exit Pattern

When a **standalone `if` with no `else`** contains a control flow exit (`return`, `return expr`, `break`, or `continue`), the compiler applies the **inverse** of the condition to the rest of the enclosing scope.

> [!WARNING]
> Inverse narrowing only applies to standalone `if` — no `else`
> The moment an `else` or `else if` is present, the compiler cannot guarantee which branch ran. The exit may have come from the `if` branch or never fired at all. Inverse narrowing is therefore **not applied** after an `if-else` chain.
>
> ```luc
> -- VALID: standalone if — inverse narrowing applies
> if a == nil { return }
> -- a is non-nullable here
>
> -- INVALID: has else — inverse narrowing NOT applied after the chain
> if a == nil { return } else { io.printl("not nil") }
> -- a is still int? here
>
> -- INVALID: chained else-if — inverse narrowing NOT applied after the chain
> if a == nil { return } else if b == nil { return }
> -- a and b are still nullable here — compiler cannot know which branch ran
> ```

The condition determines what gets narrowed and in which direction:

| Condition  | Inside block        | Rest of scope (inverse)      |
| ---------- | ------------------- | ---------------------------- |
| `a == nil` | `a` is nil          | `a` is non-nullable          |
| `a != nil` | `a` is non-nullable | `a` is nullable (no change)  |
| `not a`    | `a` is nil or false | `a` is non-nullable          |
| `a is T`   | `a` is `T`          | `a` is not `T` (still `any`) |
| `a is T?`  | `a` is `T?`         | `a` is not `T?`              |

```luc
-- guard: exit on nil → rest of scope is non-nullable
if a == nil { return }       -- rest: a is int
if not a    { return }       -- rest: a is int  (lua-style nil/false check)

-- guard: exit on non-nil → no narrowing gained after exit
if a != nil { return }       -- rest: a is int? (unchanged)
```

**`or` at the top level — each sub-condition narrowed independently:**

When conditions are joined by `or`, the exit fires if ANY is true. The inverse is ALL negated — every sub-condition's inverse is safely applied:

```luc
if a == nil or b == nil { return }
-- inverse: a != nil AND b != nil
-- rest: a is int, b is string — both narrowed

if a == nil or b == nil or c == nil { return }
-- rest: a, b, c all non-nullable

-- mixed: nil check + comparison — both sides contribute
if a == nil or x > 0 { return }
-- inverse: a != nil AND x <= 0
-- rest: a is int (type narrowed), x <= 0 (value constraint)

-- mixed: nil check + is — both sides contribute
if a == nil or x is string { return }
-- inverse: a != nil AND x is not string
-- rest: a is int (narrowed), x is not string (still any)
```

**`and` at the top level — narrowing is unsound, not applied:**

When conditions are joined by `and`, the exit fires only if ALL are true. The inverse is `or` — at least one condition is false, but the compiler cannot know which. Narrowing any single variable would be unsound:

```luc
if a == nil and b == nil { return }
-- inverse: a != nil OR b != nil
-- only one is guaranteed non-nil — cannot narrow either safely
-- no narrowing applied when 'and' is at the top level
```

**Nested `and`/`or` — compiler walks until it hits `and`:**

```luc
if (a == nil and b == nil) or c == nil { return }
-- left sub-expr: AND at top level → a and b not narrowed
-- right sub-expr: c == nil → c narrowed
-- rest: only c is non-nullable
```

**`is` narrowing with exit:**

```luc
let x any = getValue()

if x is int  { return }    -- rest: x is not int  (still any)
if x is int? { return }    -- rest: x is not int?
```

Narrowing away a type is useful for early rejection but cannot assert a positive type for `any`.

> [!TIP]
> Inverse narrowing patterns to know
>
> **Flat nil guards at the top of a function** — eliminates deeply nested blocks and makes preconditions visible at a glance:
>
> ```luc
> let process (a int?, b string?, c User?) -> int = {
>     if a == nil or b == nil or c == nil { return -1 }
>     -- from here: a is int, b is string, c is User
>     return a + b:len() + c.id
> }
> ```
>
> **Loop body guards** — skip nil elements without nesting:
>
> ```luc
> for item int? in items {
>     if item == nil { continue }
>     -- item is int for the rest of this iteration
>     process(item)
> }
> ```
>
> **Mixed conditions are fine with `or`** — comparisons and `is` checks alongside nil checks all contribute to narrowing:
>
> ```luc
> if a == nil or b == nil or x > 100 { return }
> -- rest: a is int, b is int, x <= 100
> ```
>
> **Stack multiple standalone guards instead of chaining else-if** — each guard independently narrows its variables:
>
> ```luc
> -- WRONG: chained else-if, no inverse narrowing after chain
> if a == nil { return } else if b == nil { return }
>
> -- CORRECT: two standalone guards, both narrow independently
> if a == nil { return }
> if b == nil { return }
> -- a is int, b is string here
> ```
>
> **Prefer `or` over `and` in guard conditions** — `and` at the top level prevents narrowing because the compiler cannot determine which variable caused the exit.

```luc
if score >= 90 {
    io.printl("A")
} else if score >= 80 {
    io.printl("B")
} else {
    io.printl("F")
}
```

#### If / Else — Common Patterns

**Single branch — no else:**

```luc
if user == nil { return }

if flags && 0x01 != 0 {
    io.printl("flag set")
}
```

**Two branches:**

```luc
let label string = ""

if score >= 60 {
    label = "pass"
} else {
    label = "fail"
}
```

**Chained else-if — evaluated top to bottom, first match wins:**

```luc
if score >= 90 {
    io.printl("A")
} else if score >= 80 {
    io.printl("B")
} else if score >= 70 {
    io.printl("C")
} else if score >= 60 {
    io.printl("D")
} else {
    io.printl("F")
}
```

**Inverse narrowing across chained else-if:**

Standard narrowing applies **inside** each branch and accumulates. After the entire chain, no inverse narrowing is applied — the compiler cannot know which branch ran:

```luc
let x any = getValue()

if x is int {
    -- x is int
} else if x is string {
    -- x is string (and not int)
} else if x is bool {
    -- x is bool (and not int, not string)
} else {
    -- x is not int, not string, not bool
}
-- after the chain: x is any again — no inverse narrowing
```

**Combining inverse narrowing with else-if:**

```luc
let a int? = getA()
let b int? = getB()

if a == nil {
    io.printl("a is nil")
} else if b == nil {
    -- a is int here (narrowed by the first condition being false)
    io.printl("b is nil, a is: " + string(a))
} else {
    -- both a and b are int here
    io.printl(string(a + b))
}
-- after the chain: a and b are int? again — no inverse narrowing applied
```

> [!NOTE]
> To narrow variables after a multi-condition check, use **stacked standalone guards** instead of `else-if`:
>
> ```luc
> -- else-if: no inverse narrowing after the chain
> if a == nil { return } else if b == nil { return }
> -- a and b still nullable here
>
> -- stacked guards: each narrows independently
> if a == nil { return }
> if b == nil { return }
> -- a is int, b is string here
> ```

> [!NOTE]
> When you need to produce a value from a branch, prefer the `if` expression form (`if cond ?? thenExpr else elseExpr`) over a multi-branch statement. Reserve the statement form for side effects, multiple statements per branch, or more than two outcomes.

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

## Nullable Chain Operator `?.`

`?.` accesses a field on a nullable value without requiring an explicit nil check first. If the value is nil, the entire chain short-circuits and produces nil. The chain **must** be terminated by `??` — the compiler enforces this. Using `?.` without a terminating `??` is a compile error.

```
nullable_chain  := expr '?.' IDENTIFIER { '?.' IDENTIFIER } '??' expr
                   -- expr before ??  : the chain — one or more ?. accesses
                   -- expr after  ??  : the fallback value when any step is nil
```

### How It Works

`?.` propagates nil through the chain lazily. Each step is evaluated only if the previous step was non-nil. The first nil encountered short-circuits the rest of the chain and the `??` fallback is produced:

```luc
struct Address { city string  zip string }
struct User    { name string  address Address? }

let user User? = getUser()

-- without ?.
let city string = ""
if user != nil and user.address != nil {
    city = user.address.city
}

-- with ?. — flat, readable, same semantics
let city string = user?.address?.city ?? ""
```

### Chaining Rules

Each `?.` step accesses one field or calls one method on the result of the previous step. The result of the entire chain before `??` is the type of the final accessed field wrapped in `?`:

```luc
let user  User?    = getUser()
let zip   string   = user?.address?.zip ?? "unknown"
--                   ^^^^^^^^^^^^^^^^^^^
--                   type before ?? is string? — nullable because chain may short-circuit

let len   int      = user?.name?.len() ?? 0     -- method call at end of chain
```

### `?.` vs `.` vs `??`

| Operator | Requires non-nil?     | Produces              |
| -------- | --------------------- | --------------------- |
| `.`      | ✅ Yes — crash if nil  | `T`                   |
| `?.`     | ❌ No — short-circuits | `T?`                  |
| `??`     | —                     | `T` (fallback if nil) |

`?.` and `??` are complementary — `?.` introduces potential nil, `??` resolves it:

```luc
struct Foo { field int }

let a Foo? = nil

a.field          -- ERROR: a is nullable, use ?. or guard first
a?.field         -- OK: produces int? — but still needs ??
a?.field ?? 0    -- OK: full chain, produces int
```

### Nullable Element Access in Arrays

`?.` also applies to array element access on nullable arrays:

```luc
type IntList = [_, int]

let arr IntList? = getList()

let first int = arr?[0] ?? -1    -- arr is nil: -1, else arr[0]
```

### Nested Structs — Full Example

```luc
struct Engine  { horsepower int  cylinders int }
struct Car     { engine Engine?  model string  }
struct Garage  { car Car?  owner string }

let garage Garage? = getGarage()

-- deeply nested nullable access — flat with ?.
let hp      int    = garage?.car?.engine?.horsepower ?? 0
let model   string = garage?.car?.model              ?? "unknown"
let owner   string = garage?.owner                   ?? "no owner"

-- partial chain — access stops at Car level
let car     Car?   = garage?.car ?? Car { engine = nil  model = "default" }
```

### With Method Calls

`?.` can appear before a method call using `:` at the end of the chain:

```luc
let user User? = getUser()

let upper string = user?.name:toUpper() ?? ""
--                 ^^^^^^^^^^ ?.  accesses name (string? if User? → name is string)
--                                :toUpper() called on string
--                                ?? resolves the nullable result
```

> [!WARNING]
> `?.` without a terminating `??` anywhere in the enclosing expression is a compile error. The compiler enforces that every nullable chain is resolved before the value is used:
>
> ```luc
> let city = user?.address?.city      -- ERROR: nullable chain not terminated by ??
> let city = user?.address?.city ?? ""  -- OK
> ```

> [!NOTE]
> `?.` only applies to field access and method calls — not to function calls on a nullable function binding. For nullable function calls, use a nil guard or the `~nullable` qualifier pattern:
>
> ```luc
> let handler ~nullable (e Event) = nil
>
> handler?.()          -- ERROR: ?. not valid for function calls
> if handler != nil { handler(e) }    -- OK: explicit guard
> ```

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

`!` binds to the immediately preceding **named or primitive** type token only. With bracket-enclosed array syntax, `!` after an array type attaches to the whole array unambiguously — the closing `]` marks the end of the array type before `!` is encountered:

```luc
[_, int]!string     -- ([_, int])!string   — slice of int, op can fail with string
[*, int]!DbError    -- ([*, int])!DbError  — dynamic array, op can fail
[_, int!string]     -- [_, (int!string)]   — slice of result elements
[_, int?]!string    -- ([_, int?])!string  — slice of nullable int, op can fail
```

Inline function types still require an alias before `!` can attach to the whole function type — the function return type bleeds into what follows, making the binding ambiguous:

```luc
-- AMBIGUOUS: does ! attach to the function type as a whole, or to its return type int?
(src string) -> int!string

-- parser reads: (src string) -> (int!string) — function returning int!string
-- to make the whole function type the success value, alias first:
type ParserFn    = (src string) -> int
let f () -> ParserFn!string = { ... }    -- (ParserFn)!string — unambiguous
```

Valid without alias — primitives, structs, enums, array types, and named aliases:

```luc
int!string           -- OK: primitive
Vec2!string          -- OK: struct
Direction!string     -- OK: enum
[_, int]!string      -- OK: bracket array — unambiguous
[*, User]!DbError    -- OK: bracket dynamic array — unambiguous
UserArray!DbError    -- OK: named alias
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

| Attribute                | Valid on                     | Purpose                                                          |
| ------------------------ | ---------------------------- | ---------------------------------------------------------------- |
| `@extern("sym")`         | `let`, `const` func/var      | Bind to C symbol by name                                         |
| `@extern("sym", "conv")` | `let`, `const` func/var      | With explicit calling convention                                 |
| `@link("lib")`           | package, file, or `const`    | Set active link context — one declaration, comma-separated paths |
| `@inline`                | func                         | Suggest always inline                                            |
| `@noinline`              | func                         | Prevent inlining                                                 |
| `@packed`                | `struct`                     | Remove padding — all fields byte-adjacent                        |
| `@deprecated("msg")`     | func, var, struct            | Emit warning at every use site                                   |
| `@phantom`               | `type` alias, `struct`, func | Allow unused generic parameters                                  |
| `@aot`                   | `main` only                  | Ahead-of-time compilation                                        |
| `@jit`                   | `main` only                  | JIT compilation                                                  |

`@inline` and `@noinline` are mutually exclusive on the same declaration.
`@aot` and `@jit` are mutually exclusive on the same declaration.

#### `@extern` Rules

`@extern` is a **linker directive** — not a C compiler. The Luc compiler generates an external symbol reference in LLVM IR and hands it to the linker. The linker resolves the symbol from whatever object files, static libraries, or dynamic libraries are provided at link time. The Luc compiler never sees or processes C source code.

**Declaration rules:**

- Requires `const`, not `let` — the linker resolves the symbol permanently.
- Functions must have no body — the compiler emits a warning if an empty body `= {}` is supplied (it is silently ignored) and an error if a non-empty body is supplied.
- Using `let` instead of `const` produces a warning.
- The symbol name must match exactly what appears in the compiled binary — C functions use their plain name, no mangling.
- `@extern` uses the active `@link` context set by the most recent `@link` declaration in the file. If no `@link` has been declared yet, the linker searches default system locations only.
- Writing `@extern` before any `@link` is valid — it simply uses the empty context (default locations). This is intentional for universally available symbols like `malloc` and `strlen`.
- Writing `@extern` after a `@link` that was intended for a different set of symbols is a common mistake — the sticky context applies to every `@extern` that follows, not just the next one.

```luc
-- CASE 1: @extern before any @link — searches default locations only
-- valid for universally available symbols
@extern("malloc")
const malloc (size uint64) -> *uint8?    -- OK: malloc is in libc, always available

@extern("vkCreateInstance")
const vkCreateInstance (...) -> uint32   -- PROBLEM: vulkan may not be in default locations
                                         -- linker error if vulkan is not system-installed

-- CASE 2: @extern after the correct @link — intended behavior
@link("vulkan")

@extern("vkCreateInstance")    -- searches ["vulkan"] then default — correct
@extern("vkDestroyInstance")   -- searches ["vulkan"] then default — correct

-- CASE 3: @extern after a @link intended for something else — common mistake
@link("sqlite3")

@extern("sqlite3_open")        -- searches ["sqlite3"] — correct

@extern("sin")                 -- searches ["sqlite3"] FIRST, then default
                               -- sin is in libm, not sqlite3 — found in default eventually
                               -- but searches sqlite3 unnecessarily
                               -- if sin were ambiguous this could resolve wrong

-- CORRECT for case 3: set the right context before the @extern
@link("m")

@extern("sin")    -- searches ["m"] then default — correct
```

> [!NOTE]
> The sticky context means `@link` placement matters. Group related `@extern` declarations together under their `@link` context. Use `@link()` to clear the context when switching to symbols that should only use default locations.

**Developer workflow:**

The developer is responsible for having the C library available at link time. The Luc compiler does not compile C code. Three common cases:

1. **System libraries** (`libc`, `libm`, `pthread`, Vulkan, OpenGL) — already compiled and installed on the system. Declare the symbols with `@extern` and set the link context with `@link`:

```luc
@link("m")

@extern("sin")
const sin (x float64) -> float64

@extern("cos")
const cos (x float64) -> float64
```

2. **Third-party libraries** — install pre-compiled `.a` or `.so`/`.dll` via a package manager, set the link context:

```luc
@link("sqlite3")

@extern("sqlite3_open")
const sqlite3Open (filename *uint8, ppDb **SqliteDb) -> int

@extern("sqlite3_exec")
const sqlite3Exec (db *SqliteDb, sql *uint8, callback *uint8, arg *uint8, errmsg **uint8) -> int
```

3. **Own C code** — compile it separately before invoking the Luc compiler, then pass the object file directly to the linker. No `@link` needed for object files passed directly:

```bash
clang -c mybridge.c -o mybridge.o
luc build main.luc mybridge.o
```

```luc
-- symbols from mybridge.o — no @link needed, object passed directly
@extern("mybridge_init")
const mybridgeInit (config *uint8) -> int

@extern("mybridge_process")
const mybridgeProcess (data *uint8, len uint64) -> *uint8
```

**Common `@extern` examples:**

```luc
-- memory allocation (libc — always available, no @link needed)
@extern("malloc")
const malloc (size uint64) -> *uint8?

@extern("free")
const free (ptr *uint8)

@extern("memcpy")
const memcpy (dst *uint8, src *uint8, n uint64) -> *uint8

-- variadic C function
@extern("printf", "C")
const printf (fmt *uint8, args ...any) -> int

-- data symbol (no parameters, no return — just a pointer)
@extern("__stack_top")
const stackTop *uint8
```

**C++ interop — heavily limited:**

C++ support is limited to functions exposed through `extern "C"` wrappers. C++ name mangling makes direct symbol binding fragile and platform-dependent. The recommended pattern is to write a thin C wrapper in C++ and declare that wrapper in Luc:

```cpp
// myclass_bridge.cpp
extern "C" {
    int myclass_process(int x) { return MyClass().process(x); }
    void myclass_destroy(MyClass* obj) { delete obj; }
}
```

```luc
@extern("myclass_process")
const myclassProcess (x int) -> int

@extern("myclass_destroy")
const myclassDestroy (obj *uint8)
```

Direct C++ symbol binding (mangled names or `"C++"` calling convention) is not supported. A better solution may be designed in a future version of Luc.

#### `@link` Rules

`@link` sets the **active link context** — an ordered list of library paths the linker searches before the default system locations. Every `@extern` that follows uses the active context until a new `@link` replaces it.

**One form only — comma-separated arguments:**

Multiple paths must be written as arguments in a single `@link`. Writing multiple `@link` lines is a compile error — consolidate them:

```luc
-- CORRECT: one @link, comma-separated paths
@link("./libs/mylib.a", "./fallback/mylib.a", "mylib")

-- ERROR: multiple @link lines
@link("./libs/mylib.a")
@link("./fallback/mylib.a")    -- ERROR: duplicate @link, use comma-separated args
```

**Sticky context — applies to all following `@extern` until replaced:**

Once set, the active link context persists across every `@extern` declaration that follows in the file. A new `@link` replaces the context entirely:

```luc
-- set context: all following @extern search vulkan first
@link("vulkan")

@extern("vkCreateInstance")     -- searches ["vulkan"] then default
@extern("vkDestroyInstance")    -- searches ["vulkan"] then default
@extern("vkSubmitQueue")        -- searches ["vulkan"] then default

-- replace context: all following @extern now search libm
@link("m")

@extern("sin")     -- searches ["m"] then default
@extern("cos")     -- searches ["m"] then default
@extern("sqrt")    -- searches ["m"] then default

-- clear context: revert to default-only search
@link()

@extern("malloc")  -- default locations only
@extern("free")    -- default locations only
```

**Search order:**

1. Paths in the active `@link` context, left to right
2. Default system locations (`/usr/lib`, `/usr/local/lib`, platform equivalents)

If the symbol is not found in any location, the linker emits a fatal build error naming the unresolved symbol and listing the paths that were searched.

**Without any `@link`:**

If no `@link` has been declared before an `@extern`, the active context is empty and the linker searches default system locations only. This is fine for universally available symbols like `malloc`, `free`, `strlen`:

```luc
-- no @link needed — malloc is in libc, always available
@extern("malloc")
const malloc (size uint64) -> *uint8?
```

**Context is file-scoped:**

The active link context does not leak across files. Each file starts with an empty context.

**Grammar:**

```
attr_link       := '@link' '(' [ link_path { ',' link_path } ] ')'

link_path       := STRING_LITERAL
                   -- "name"              — system library, linker searches standard paths
                   -- "./path/lib.a"      — relative path to static library
                   -- "./path/lib.so"     — relative path to dynamic library
                   -- "/absolute/lib.a"   — absolute path to static library
```

**Full example:**

```luc
-- Vulkan functions — search vulkan library
@link("vulkan")

@extern("vkCreateInstance")
const vkCreateInstance (
    pInfo      *VkInstanceCreateInfo,
    pAllocator *VkAllocationCallbacks,
    pInstance  **VkInstance
) -> uint32

@extern("vkDestroyInstance")
const vkDestroyInstance (instance *VkInstance, pAllocator *VkAllocationCallbacks)

@extern("vkCreateDevice")
const vkCreateDevice (
    physDevice *VkPhysicalDevice,
    pInfo      *VkDeviceCreateInfo,
    pAllocator *VkAllocationCallbacks,
    pDevice    **VkDevice
) -> uint32

-- math functions — replace context with libm
@link("m")

@extern("sin")
const sin (x float64) -> float64

@extern("cos")
const cos (x float64) -> float64

-- SQLite with fallback paths — multiple args, left-to-right search
@link("./libs/sqlite3.a", "/usr/local/lib/libsqlite3.a", "sqlite3")

@extern("sqlite3_open")
const sqlite3Open (filename *uint8, ppDb **SqliteDb) -> int

@extern("sqlite3_exec")
const sqlite3Exec (
    db       *SqliteDb,
    sql      *uint8,
    callback *uint8,
    arg      *uint8,
    errmsg   **uint8
) -> int

-- clear context — back to default locations only
@link()

@extern("malloc")
const malloc (size uint64) -> *uint8?

@extern("free")
const free (ptr *uint8)
```

Library name forms:

| Form                        | Meaning                                                    |
| --------------------------- | ---------------------------------------------------------- |
| `"m"`                       | System library — linker searches standard paths for `libm` |
| `"sqlite3"`                 | System library — linker searches for `libsqlite3`          |
| `"./libs/mylib.a"`          | Relative path to static library                            |
| `"./libs/libmylib.so"`      | Relative path to dynamic library                           |
| `"/usr/local/lib/libpng.a"` | Absolute path to static library                            |

#### `@phantom` Rules

`@phantom` suppresses the unused-type-parameter compile error. It is valid on three declaration kinds:

- **Type aliases** — a type parameter that does not appear in the alias body
- **Structs** — a type parameter that does not appear in any field declaration
- **Functions** — a type parameter that does not appear in any parameter type or return type

Without `@phantom`, an unused type parameter is a **compile error** on all three. With `@phantom`, unused parameters are permitted — the compiler treats them as phantom tags that exist only at the type-checking level and are erased at runtime. Phantom parameters still participate in type identity.

```luc
-- type alias
@phantom
type Tagged<T> = int         -- OK: T is a phantom tag
type Gen<T>    = int         -- ERROR: T is unused — add @phantom if intentional

-- struct
@phantom
struct Marker<T> {
    raw int                  -- OK: T is a phantom tag
}
struct Bad<T> {
    raw int                  -- ERROR: T is unused — add @phantom if intentional
}

-- function
@phantom
let makeToken<T> (name string) -> string = {
    return name              -- OK: T used only as a type tag at call site
}
let bad<T> (name string) -> string = {
    return name              -- ERROR: T is unused — add @phantom if intentional
}

-- phantom parameters still affect type identity
let intToken    string = makeToken<int>("id")
let stringToken string = makeToken<string>("id")
-- makeToken<int> and makeToken<string> are distinct instantiations
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
| `#toRef(ptr)`        | `*T`       | `&T`    | Assert valid, cross to safe reference |
| `#toPtr(ref)`        | `&T`       | `*T`    | Convert reference to raw pointer      |
| `#ptrOffset(ptr, n)` | `*T`, int  | `*T`    | Pointer arithmetic (element offset)   |
| `#ptrDiff(p1, p2)`   | `*T`, `*T` | `int64` | Distance between pointers in elements |

These intrinsics are the only way to cross the sealed conduit boundary or perform pointer arithmetic.

```luc
let buf  *uint8 = malloc(1024)
let ref  &uint8 = #toRef(buf)
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