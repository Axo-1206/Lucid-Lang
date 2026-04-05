# Luc — Annotated Code Examples

> **Scope of this file:** Practical, annotated examples for every major language construct.
> Grammar rules are in `LUC_GRAMMAR.md`. Project identity is in `LUC_PROJECT_OVERVIEW.md`.

# DEPRECATED DO NOT REFERENCE THIS FILE
# DEPRECATED DO NOT REFERENCE THIS FILE
# DEPRECATED DO NOT REFERENCE THIS FILE
# DEPRECATED DO NOT REFERENCE THIS FILE
# DEPRECATED DO NOT REFERENCE THIS FILE

## Table of Contents

1. [Package & Module System](#1-package--module-system)
2. [Variable Declarations](#2-variable-declarations)
3. [Types](#3-types)
4. [Functions](#4-functions)
5. [Enum](#5-enum)
6. [Struct](#6-struct)
7. [Type Aliases](#7-type-aliases)
8. [Trait](#8-trait)
9. [Impl Block](#9-impl-block)
10. [Type Conversion — `from`](#10-type-conversion--from)
11. [If / Else](#11-if--else)
12. [Match](#12-match)
13. [Switch](#13-switch)
14. [Loops](#14-loops)
15. [Compound Assignment](#15-compound-assignment)
16. [Type Checking — `is`](#16-type-checking--is)
17. [Nullable Chains `.?` and `??`](#17-nullable-chains--and-)
18. [Pipeline Operator `->`](#18-pipeline-operator--)
19. [Composition Operator `+>`](#19-composition-operator-)
20. [Currying](#20-currying)
21. [Closures](#21-closures)
22. [Async / Await](#22-async--await)
23. [Parallel](#23-parallel)
24. [FFI / Extern](#24-ffi--extern)
25. [Doc Comments](#25-doc-comments)
26. [Vulkan Example](#26-vulkan-example)

## 1. Package & Module System

Every `.luc` file is a module. Its identity is its file path relative to the
package root — no declaration is needed inside the file. Modules are flat.

Visibility is controlled by top-level modifiers:

| Keyword | Scope | Access |
|---|---|---|
| (none) | **File** | Only visible within the same `.luc` file. |
| `pub` | **Package** | Visible to other files sharing the same `package` name. |
| `export` | **World** | Visible to external consumers of the package. |

```luc
-- math/vec2.luc
package math

-- pub = visible to other files in this package
pub struct Vec2 { x float  y float }

-- export = visible to consumers importing this package
export struct Mat4 { ... }

-- pub impl = methods callable by anyone holding a Vec2 (internal to package)
pub impl Vec2 {
    length () float = { ... }
}

-- export impl = methods callable externally
export impl Mat4 {
    identity () Mat4 = { ... }
}

-- bare impl = callable only within the math/vec2.luc file
impl Vec2 {
    validate () bool = { ... }
}

-- file-private: no pub, not visible outside this file
imt PI float = 3.14159
```

An optional API manifest can be created using `export use` to re-export `pub` declarations as `export`:

```luc
-- math/api.luc  (optional manifest — curated public API)
package math

export use math.vec2    -- promotes all pub items in vec2 to export
export use math.matrix.Mat2
```

```luc
-- consumer.luc
package app

use math             -- gets only what is marked 'export'
use math.vec2        -- import a specific file directly
use math as m        -- local alias: m.Vec2
```

> **Rules**
> - One `package` declaration per file, first non-comment line.
> - Module paths use `.` as the separator: `math.vec2`.
> - If a package has *zero* `export` modifiers, it is considered a **Private Package** (nothing is implicitly exported).
> - Circular imports are a semantic error.

## 2. Variable Declarations

```luc
-- let: reassignable, mutable in place, nil allowed
let count int = 0
let name string? = nil     -- nil is valid for let
count = count + 1          -- reassignment OK
count += 1                 -- compound assign OK

-- imt: not reassignable, not mutable in place, nil allowed
imt maxRetries int = 5
-- maxRetries = 10          -- ERROR: imt cannot be reassigned

-- val: not reassignable, not mutable in place, nil forbidden in entire type tree
val gravity float = 9.81
-- val broken int? = nil    -- ERROR: val forbids nil anywhere in its type tree
```

| Keyword | Reassignable | Mutable in place | Nil allowed |
|---|---|---|---|
| `let` | ✅ | ✅ | ✅ |
| `imt` | ❌ | ❌ | ✅ |
| `val` | ❌ | ❌ | ❌ (entire type tree) |

> **Note:** Type annotation goes directly after the name — no `:` separator.
> The annotation is always required in declarations.

## 3. Types

### Primitives

```luc
let a bool    = true
let b byte    = 127           -- int8,  -128..127
let c short   = -32000        -- int16
let d int     = -1000000      -- int32
let e long    = 9000000000    -- int64
let f ubyte   = 255           -- uint8, 0..255
let g uint    = 4000000000    -- uint32
let h float   = 1.5           -- 32-bit
let i double  = 3.14159265    -- 64-bit
let j decimal = 1.23456789012 -- 128-bit, high precision
let k string  = "hello"
let l char    = 'L'
```

### Fixed-width types (Vulkan-critical)

```luc
let flags  uint32 = 0xFF00FF00
let handle uint64 = 0x0000000000000001
let bits   int8   = -128
let mask   uint16 = 0b1111000011110000
```

### Nullable

```luc
-- ? suffix attaches directly to the type
let x int? = nil
let s string? = "present"

-- Nullable return: ? is on the return type, outside the parens
let parse (src string) int? = { ... }

-- Nullable function: ? wraps the entire function type
let handler ((req string) int)? = nil
-- handler itself may be nil; when non-nil it is called as (string) int
```

### Union

```luc
let id int | string = 42
let id2 int | string = "user-abc"
```

### Reference and raw pointer

```luc
-- &T — safe managed reference (general use)
let r &int = &count

-- @T — raw pointer, only valid in extern / FFI declarations (see §24)
```

### Arrays

```luc
let nums []int = [1, 2, 3, 5, 8]          -- dynamic
let rgba [4]float = [1.0, 0.5, 0.0, 1.0]  -- fixed-size
let grid [][]float = [[1.0, 0.0], [0.0, 1.0]]  -- multidimensional

let first int   = nums[0]    -- 1
let r     float = rgba[0]    -- 1.0
```

## 4. Functions

Functions are **first-class values**. A function declaration is syntactic sugar
for assigning a function body to a variable.

### Basic forms

```luc
-- Shorthand block body (preferred)
let add (a int b int) int = {
    return a + b
}

-- imt: permanently bound, cannot be reassigned
imt double (n int) int = { return n * 2 }

-- let: body can be reassigned
let transform (n int) int = { return n * 2 }
transform = { return n + 100 }   -- rebind to a new body

-- No return type (void equivalent)
let greet (name string) = {
    io.printl("Hello, " + name)
}

-- Explicit anonymous function form (identical semantics, more verbose)
let multiply (a int b int) int = (a int b int) int {
    return a * b
}
```

### Nullable return

```luc
-- ? on the return type — the function may return nil
let findUser (id int) string? = {
    if id == 0 { return nil }
    return "user-" + string(id)
}
```

### Variadic parameters

```luc
let sumAll (args ...int) int = {
    let total int = 0
    for n in args { total += n }
    return total
}

sumAll(1, 2, 3)    -- 6
```

### `if` as a direct body

```luc
-- else is required; both branches must return the same type
let sign (n int) string = if n >= 0 { "positive" } else { "negative" }
```

### `match` as a direct body

```luc
pub enum Direction { North  South  East  West }

let describe (dir Direction) string = match dir {
    Direction.North -> "north"
    Direction.South -> "south"
    Direction.East  -> "east"
    Direction.West  -> "west"
    default         -> "unknown"
}
```

### `async` sugar body

```luc
-- async { } inherits params and return type from the enclosing signature
let fetch (url string) string = async {
    return await httpGet(url)
}

-- Explicit form (equivalent, more verbose)
let fetch (url string) string = async (url string) string {
    return await httpGet(url)
}
```

### First-class usage

```luc
-- Assign a named function to a variable
let op (a int b int) int = add
let result int = op(3, 7)    -- 10

-- Pass as an argument
let applyTwice (f (n int) int, x int) int = { return f(f(x)) }
applyTwice(double, 3)    -- 12

-- Nullable function type — outer parens wrap the whole signature, then ?
let callback ((n int) string)? = nil
```

## 5. Enum

An enum is a named, closed set of constants backed by an integer.
Variants are always accessed via `EnumName.Variant` dot syntax.

```luc
pub enum Direction {
    North           -- auto-assigned 0
    South           -- 1
    East            -- 2
    West            -- 3
}

-- Explicit integer values (useful for Vulkan flag bits)
pub enum ShaderStage {
    Vertex   = 0x01
    Fragment = 0x02
    Compute  = 0x04
    Geometry = 0x08
}

-- Mixed: some explicit, rest continue incrementing from last explicit
pub enum Priority {
    Low             -- 0
    Medium          -- 1
    High     = 10   -- 10
    Critical        -- 11
}
```

### Enum usage

```luc
let dir   Direction   = Direction.North
let stage ShaderStage = ShaderStage.Fragment

-- Compare only to same enum type
if dir != Direction.South {
    move(dir)
}

-- Explicit cast when integer value is needed
let raw int = int(stage)   -- 2

-- is operator with enum variant
if stage is ShaderStage.Fragment {
    bindFragmentShader()
}

-- match on enum
let label (stage ShaderStage) string = match stage {
    ShaderStage.Vertex   -> "vertex"
    ShaderStage.Fragment -> "fragment"
    ShaderStage.Compute  -> "compute"
    ShaderStage.Geometry -> "geometry"
    default              -> "unknown"
}
```

## 6. Struct

```luc
-- Basic struct: field name then type, no colon separator
struct Vec2 {
    x float
    y float
}

-- Fields with default values
struct Color {
    r float = 1.0
    g float = 1.0
    b float = 1.0
    a float = 1.0
}

-- Public struct
pub struct Vertex {
    pos   Vec3   -- world-space position
    color Vec4   -- RGBA, linear color space
    uv    Vec2   -- texture coordinates, 0..1
}

-- Generic struct (unconstrained)
struct Pair<A B> {
    first  A
    second B
}

-- Generic with single trait constraint
struct Scene<T : Drawable> {
    objects []T
}

-- Generic with multiple constraints
struct Cache<K : Hashable + Comparable, V> {
    keys   []K
    values []V
}
```

### Struct literals

```luc
let origin Vec2  = Vec2 { x = 0.0  y = 0.0 }
let white  Color = Color {}                         -- all defaults
let p Pair<int string> = Pair<int string> { first = 1  second = "one" }

let px float = origin.x    -- 0.0
```

## 7. Type Aliases

`type` introduces a name for an existing shape. `=` is always required.
Inline struct bodies are not allowed on the right-hand side.

```luc
-- Primitive alias
type ID    = int
type Score = float

-- Named alias (ID and int are interchangeable at semantic level)
type UserID = ID

-- Union alias
type Number = int | float
type Result = string | int | nil

-- Array alias
type Matrix  = [][]float
type ByteBuf = []byte

-- Reference alias
type IntRef = &int

-- Function type alias
type Callback     = (event Event) bool
type Handler      = (req Request) Response?
type Transform<T> = (value T) T

-- Nullable function alias — the function itself may be nil
type MaybeHandler = ((req Request) Response)?

-- Raw pointer alias — FFI only
type RawBuf = @uint8
```

## 8. Trait

A trait is a pure method contract — signatures only, no fields, no bodies.

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

> **Rules**
> - Traits are top-level only — never nested.
> - No field declarations, no default implementations.
> - Conformance is explicit: `: TraitName` is required on the impl block.

## 9. Impl Block

`impl` binds methods to a struct. No `let`/`imt`/`val` inside — method
declaration syntax only. Visibility is controlled at the block level, not
per method.

```luc
struct Vec2 {
    x float
    y float
}

-- pub impl: methods callable by anyone holding a Vec2
pub impl Vec2 {
    length () float = { return (x*x + y*y) -> sqrt }

    dot (other Vec2) float = { return x*other.x + y*other.y }

    scale (factor float) Vec2 = {
        return Vec2 { x = x * factor  y = y * factor }
    }

    normalize () Vec2 = {
        let len float = length()
        return scale(1.0 / len)
    }
}

-- Multiple pub impl blocks merge at semantic time (no duplicate method names)
pub impl Vec2 {
    add (other Vec2) Vec2 = {
        return Vec2 { x = x + other.x  y = y + other.y }
    }
}

-- bare impl: callable only within the package
impl Vec2 {
    isZero () bool = { return x == 0.0 and y == 0.0 }
}
```

### Trait conformance

```luc
pub struct Circle {
    center Vec2
    radius float
}

pub impl Circle : Drawable {
    draw   ()      = { ... }
    bounds () Rect = { ... }
}

-- Multiple traits across separate impl blocks
pub impl Circle : Hashable {
    hash   () uint64        = { ... }
    equals (other any) bool = { ... }
}
```

### Generic impl

```luc
pub impl<T : Drawable> Scene<T> {
    drawAll () = {
        for obj in objects { obj.draw() }
    }
}
```

### Using impl methods

```luc
let v Vec2 = Vec2 { x = 3.0  y = 4.0 }

let len    float = v.length()      -- 5.0
let scaled Vec2  = v.scale(2.0)    -- Vec2 { x = 6.0, y = 8.0 }
let d      float = v.dot(scaled)   -- 50.0
```

## 10. Type Conversion — `from`

`from` inside a `pub impl` block defines how to construct this type from
another type. The call-site syntax is `TargetType(expr)`.

```luc
struct Celsius    { value float }
struct Fahrenheit { value float }
struct Kelvin     { value float }

pub impl Fahrenheit {
    -- Convert from Celsius
    from (c Celsius) Fahrenheit = {
        return Fahrenheit { value = c.value * 9.0 / 5.0 + 32.0 }
    }

    -- Multiple from declarations — each accepts a different source type
    from (k Kelvin) Fahrenheit = {
        return Fahrenheit { value = (k.value - 273.15) * 9.0 / 5.0 + 32.0 }
    }
}

-- Call site: TypeName(expr) dispatches to the matching from block
let boiling Celsius    = Celsius    { value = 100.0 }
let hot     Fahrenheit = Fahrenheit(boiling)         -- calls from(Celsius)

let abs  Kelvin     = Kelvin     { value = 373.15 }
let absF Fahrenheit = Fahrenheit(abs)                -- calls from(Kelvin)

-- Works as a pipeline step
boiling -> Fahrenheit -> io.printl
```

### Primitive type conversion

Primitive type names are callable as safe conversion functions:

```luc
let n int    = 42
let f float  = float(n)      -- safe: int → float
let s string = string(n)     -- safe: int → string

-- In a pipeline
42 -> float -> sqrt           -- int literal → float → sqrt
x  -> string -> io.printl           -- x converted to string, then passed to io.printl
```

### Unsafe bit reinterpret (`@`)

```luc
-- @prefix = reinterpret bits, no arithmetic, FFI / Vulkan only
bits -> @float                -- reinterpret uint32 bits as float32
rawBytes -> @GpuVertex        -- reinterpret raw memory as GpuVertex
```

## 11. If / Else

```luc
let score int = 85

-- Statement form — else is optional
if score >= 90 {
    io.printl("A")
} else if score >= 80 {
    io.printl("B")
} else {
    io.printl("F")
}

-- Early return / guard pattern
let user string? = findUser(0)
if user == nil {
    return
}

-- Expression form — else required, both branches must return the same type
let grade string = if score >= 60 { "pass" } else { "fail" }
```

## 12. Match

`match` is **expression-oriented** — it produces a value.
`default` is required and must be the last arm.

### Value pattern

```luc
let label string = match status {
    200     -> "ok"
    404     -> "not found"
    500     -> "error"
    default -> "unknown"
}
```

### Multiple values per arm

```luc
let category string = match code {
    200, 201, 202 -> "success"
    400, 401, 403 -> "client error"
    500, 502, 503 -> "server error"
    default       -> "other"
}
```

### Range pattern

```luc
let severity string = match damage {
    0       -> "none"
    1..10   -> "light"
    11..50  -> "moderate"
    default -> "critical"
}
```

### Bind pattern with guard

```luc
-- A bare IDENTIFIER in pattern position captures the matched value
let label string = match score {
    n if n < 0  -> "invalid: " + string(n)
    n if n < 50 -> "fail: "    + string(n)
    n if n < 80 -> "pass: "    + string(n)
    n           -> "merit: "   + string(n)  -- guard-free bind: matches everything
    default     -> "unknown"
}
```

### Wildcard

```luc
-- _ discards the matched value; default is still required as the final arm
let msg string = match value {
    0       -> "zero"
    _       -> "non-zero"
    default -> "fallback"
}
```

### Struct destructuring

```luc
let desc string = match point {
    Vec2 { x: 0.0, y: 0.0 } -> "origin"
    Vec2 { x, y }            -> "at " + string(x) + ", " + string(y)
    default                  -> "unknown"
}

-- Nested struct destructuring
let info string = match player {
    Player { health: 0 }                             -> "dead"
    Player { pos: Vec2 { x: 0.0, y: 0.0 }, health } -> "at origin, hp: " + string(health)
    Player { pos, health }                           -> "alive, hp: "     + string(health)
    default                                          -> "unknown"
}
```

### Type pattern (`v is Type`)

```luc
type Shape = Circle | Rect | Triangle

let area (shape Shape) float = match shape {
    s is Circle   -> s.radius * s.radius * 3.14159
    s is Rect     -> s.width * s.height
    s is Triangle -> s.base * s.height / 2.0
    default       -> 0.0
}

-- Dispatch on any
let describe (value any) string = match value {
    v is int    -> "int: "    + string(v)
    v is string -> "string: " + v
    v is Vec2   -> "vec2"
    default     -> "unknown"
}
```

### Block arm

```luc
-- An arm body can be a block when multiple statements are needed
let response string = match code {
    200 -> {
        io.printl("success")
        return "OK"
    }
    default -> "error"
}
```

### Array matching (bind + guard — no array destructure syntax)

```luc
match items {
    arr if arr.len() == 0 -> "empty"
    arr if arr.len() == 1 -> "single: " + string(arr[0])
    arr                   -> "many: "   + string(arr.len())
    default               -> "unknown"
}
```

---

## 13. Switch

`switch` is **statement-oriented** — it runs blocks, does not produce a value.
No fallthrough; `default` is optional.

```luc
-- Multiple values per case
switch code {
    case 200, 201, 202: { io.printl("success") }
    case 400, 401:      { io.printl("client error") }
    case 500:           { io.printl("server error") }
    default:            { io.printl("unknown") }
}

-- Range per case
switch damage {
    case 0:       { io.printl("no damage") }
    case 1..10:   { io.printl("light") }
    case 11..50:  { io.printl("moderate") }
    default:      { io.printl("critical") }
}

-- Mixed values and ranges (key codes, Vulkan enums, etc.)
switch key {
    case 0x41, 0x42:  { handleAB() }
    case 0x30..0x39:  { handleDigit() }
    default:          { handleOther() }
}
```

> Use `match` when you need a value back; use `switch` for side-effect dispatch.

## 14. Loops

```luc
-- Iterative range loop with step and type
for i double in 0.0 .. 1.0 .. 0.1 {
    render_at(i)
}

-- Range with dynamic boundaries and step
for i int in 0 .. end_val .. 1 {
    render_at(i)
}

-- Foreach loop with explicit type
for i Food in menu {
    render_at(i)
}

-- while
let n int = 0
while n < 5 {
    n += 1
}

-- do-while (body always executes at least once)
let retries int = 0
do {
    retries += 1
} while retries < 3

-- break and continue
for i in 0..100 {
    if i == 50 { break }
    if i % 2 == 0 { continue }
    io.printl(string(i))    -- prints odd numbers below 50
}
```

---

## 15. Compound Assignment

`x += expr` desugars to `x = x + expr`. The LHS must be a `let` variable
or a mutable field; `imt` and `val` are not reassignable.

```luc
let x int = 10

x += 5     -- 15
x -= 3     -- 12
x *= 2     -- 24
x /= 4     -- 6
x ^= 2     -- 36  (power, desugars to x = x ^ 2)
x %= 7     -- 1   (modulo)

-- String: += is valid because + is defined on string
let name string = "hello"
name += " world"    -- "hello world"

-- Field access on LHS
obj.health -= damage
obj.score  += bonus

-- imt and val on the LHS are semantic errors
-- imt PI float = 3.14
-- PI += 1.0               -- ERROR: imt is not reassignable
```

## 16. Type Checking — `is`

`is` produces `bool` and narrows the type inside the enclosing `if` block.

```luc
-- Primitive check
if x is int {
    let doubled int = x * 2    -- x is known int here
}

-- Struct check
if shape is Circle {
    let area float = shape.radius * shape.radius * 3.14159
}

-- any type check
let value any = getData()
if value is Vec2 {
    io.printl(string(value.x))    -- value is Vec2 here
}

-- Enum variant check
if stage is ShaderStage.Fragment {
    bindFragmentShader()
}

-- Combined with logical operators
if value is int and value > 0 {
    io.printl("positive int")
}
```

## 17. Nullable Chains `.?` and `??`

`.?` propagates `nil` through a chain. `??` terminates the chain with a
non-nullable fallback. Any `.?` chain **must** be terminated by `??`.

```luc
struct Weapon { damage int  name string }
struct Player { name string  weapon Weapon? }

let player Player? = getPlayer(id)

-- If player or weapon is nil, the whole chain yields nil; ?? provides the default
let dmg int = player.?weapon.?damage ?? 0

-- Without chaining you would write:
-- if player != nil and player.weapon != nil { dmg = player.weapon.damage }

let weaponName string = player.?weapon.?name ?? "fists"

-- Standalone . (non-nullable field access) does NOT need ??
let p Player = Player { name: "hero"  weapon: nil }
let heroName string = p.name    -- OK; p is non-nullable

-- Chain feeding a pipeline step
let display string = player.?weapon.?damage ?? 0 -> string -> io.printl

-- Deeper chain
let displayName string = getSession(token).?user.?profile.?displayName ?? "anonymous"
```

> **Rules**
> - `.?` is only valid on nullable types — applying it to a non-nullable is a semantic error.
> - `??` must terminate every `.?` chain.
> - `val` forbids `?` anywhere in its type tree.

## 18. Pipeline Operator `->`

`->` executes a chain left-to-right at runtime. The **seed** can be any
expression; each **step** must be a callable identifier or anonymous function.

```luc
-- Seed is a literal
42 -> float -> sqrt

-- Seed is a variable
x -> double -> string

-- Seed is a function call result
getUser(id) -> validate -> save

-- As a statement (result discarded)
processFrame() -> submitQueue -> present

-- As an expression (capture result)
let result int = f1(args) -> f2 -> f3

-- Wrapped in a function — deferred execution
let runPipeline (input int) int = { f1(input) -> f2 -> f3 }
```

### Control flow around a pipeline

```luc
-- -> lives inside a block; you can gate it with normal control flow
imt maxCalls int = 5
let callCount int = 0

let call (args any) = {
    if callCount >= maxCalls {
        return              -- skips the pipeline entirely
    }
    callCount += 1
    f1(args) -> f2
}
```

### Step that ignores the previous result

```luc
-- If a step takes no parameters, the previous result is silently discarded
let f1 (args ...any) int = { return 42 }
let f2 () = { io.printl("step two") }

let chain (args ...any) = { f1(args) -> f2 }
```

### Anonymous function as a step

```luc
-- The anonymous function must declare a parameter to receive the previous value
-- It is a step, not a seed
f1(args)
-> (result int) string {
    if result > 0 { return result -> string }
    else          { return "empty" }
}
-> io.printl
```

## 19. Composition Operator `+>`

`+>` wires functions together **at compile time** without executing them,
producing a new function. The output type of the left must exactly match
the input type of the right.

```luc
let doubleNum   (x int)    int    = { return x * 2 }
let intToString (x int)    string = { return string(x) }
let logStr      (s string)        = { io.printl(s) }

-- Compose into a new function (not executed here)
let process = doubleNum +> intToString +> logStr

-- Call it like any function
process(21)    -- logs "42"

-- Type mismatch is a compile error
-- let bad = logStr +> doubleNum   -- ERROR: logStr returns void, doubleNum takes int

-- Feed a composed result into a -> pipeline
f1(args) -> doubleNum +> intToString -> logStr
```

### Generic functions and `+>`

Both sides must be concrete at compose time.

```luc
-- Concrete: valid
let process = doubleNum +> intToString

-- Generic: explicit instantiation required
let process<T : Numeric> = double<T> +> stringify<T>

-- Prefer -> in a wrapper when you want to avoid explicit instantiation
let process<T : Numeric> (x T) string = { double(x) -> stringify }
```

## 20. Currying

Luc uses **chained parameter groups** as its single currying model.
Declaring multiple `()` groups is syntactic sugar for a function returning
a function. No `_` placeholder model exists.

```luc
-- Two-group: classic curry
let add (a int) (b int) int = { return a + b }

add(10)        -- partial: returns (b int) int — not yet executed
add(10)(5)     -- full:    15

-- Bind a partial application to a name
let addTen = add(10)    -- (b int) int
addTen(3)               -- 13
addTen(7)               -- 17
```

### Three groups

```luc
let clamp (lo int) (hi int) (value int) int = {
    if value < lo { return lo }
    if value > hi { return hi }
    return value
}

let clampPositive = clamp(0)        -- (hi int) (value int) int
let clamp0to100   = clamp(0)(100)   -- (value int) int
clamp0to100(42)                     -- 42
clamp0to100(150)                    -- 100
```

### Currying with `->` and `+>`

```luc
let addTen = add(10)             -- (b int) int
42 -> addTen                     -- 52

let process = add(10) +> string  -- (b int) string
process(5)                       -- "15"
```

### What the compiler desugars to (internal — do not write this)

```luc
-- What you write:
let add (a int) (b int) int = { return a + b }

-- What the compiler produces internally:
let add (a int) (b int) int = { return (b int) int { return a + b } }
--                                      anonymous function capturing 'a'
```

## 21. Closures

Every partial application of a curried function produces a closure —
the returned function captures the already-supplied arguments.

```luc
let add (a int) (b int) int = { return a + b }

let addTen = add(10)    -- closure: a = 10 is fixed
addTen(5)               -- 15
addTen(20)              -- 30
```

### Closing over a local variable

When you need to capture something from the surrounding block scope (not
a function parameter), write the anonymous function explicitly — chained
groups only work for capturing the function's own parameters.

```luc
let multiplier int = 3

-- Must write an explicit anonymous function to close over a local variable
let scale = (x int) int { return x * multiplier }

scale(5)     -- 15
scale(10)    -- 30
```

## 22. Async / Await

```luc
-- async sugar body — inherits params and return type from the signature
let fetch (url string) string = async {
    let data string = await httpGet(url)
    return data
}

-- Explicit anonymous form (equivalent, more verbose)
let fetch (url string) string = async (url string) string {
    let data string = await httpGet(url)
    return data
}
```

### Composing async I/O and parallel CPU work

```luc
let process (items []Item) []Result = async (items []Item) []Result {
    -- I/O phase: async and sequential
    let raw []RawData = await fetchAll(items)

    -- CPU phase: parallel and synchronous — await is NOT valid inside parallel
    parallel for item in raw {
        item.value = item.value -> transform -> normalize
    }

    return raw -> toResults
}
```

> **Rules**
> - `await` is only valid inside an `async` body.
> - A non-async caller receives the future, not the resolved value.
> - `await` inside `parallel for` or `parallel { }` is a semantic error.

## 23. Parallel

Two forms: data-parallel iteration and task-parallel blocks.

### `parallel for` — data parallel (DOD)

```luc
-- Each element is processed independently and simultaneously
parallel for vertex in mesh.vertices {
    vertex.pos    = vertex.pos    -> transform
    vertex.normal = vertex.normal -> normalize
    vertex.uv     = vertex.uv     -> wrapUV
}

-- Range-based index iteration
parallel for i in 0..particles.len() {
    particles[i].pos += particles[i].velocity * dt
}
```

### `parallel { }` — task parallel

```luc
-- Each sub-block runs as an independent concurrent task
-- All complete before execution continues past the parallel block
parallel {
    { imt textures = loadTextures(paths)    }
    { imt shaders  = compileShaders(sources) }
    { imt meshes   = loadMeshes(files)      }
}
```

### Parallel scope rules

```luc
let total int = 0
parallel for n in numbers {
    -- total += n    -- ERROR: writing shared mutable state
    -- return        -- ERROR: cannot return from parallel body
    -- break         -- ERROR: cannot break from parallel body
    io.printl(string(n))   -- OK: reading outer variables is fine
}
```

## 24. FFI / Extern

`extern` declares an external C or Vulkan symbol. The body is omitted.
`@T` raw pointer is **only valid** inside `extern` declarations.

```luc
-- C allocator
extern let malloc (size uint64) @uint8
extern let free   (ptr @uint8)

-- Vulkan function
extern let vkCreateInstance (
    pCreateInfo @VkInstanceCreateInfo
    pAllocator  @VkAllocationCallbacks
    pInstance   @VkInstance
) uint32

-- Wrapping extern in a safe function
let createBuffer (size uint64) []byte = {
    let raw @uint8 = malloc(size)
    return wrapBuffer(raw, size)
}

-- @T in a non-extern context is a semantic error
-- let p @int = ...   -- ERROR: raw pointer only valid in extern declarations
```

## 25. Doc Comments

Three forms — all stored in the AST and shown in LSP hover.

### Form 1 — stacked `--` lines above a declaration

```luc
-- Normalizes the vector in place.
-- Only call this after the vector has been validated.
let normalize (v Vec2) Vec2 = { ... }
```

A blank line breaks the attachment:

```luc
-- This comment is floating — NOT attached to x.

let x int = 5    -- x has no doc comment
```

### Form 2 — trailing `--` on the same line

```luc
struct Vertex {
    pos   Vec3   -- world-space position
    color Vec4   -- RGBA, linear color space
    uv    Vec2   -- texture coordinates, 0..1
}

imt MAX_VERTS int = 65536   -- Vulkan hard limit on this device
```

When both stacked and trailing are present, stacked wins:

```luc
-- This is the doc.
let x int = 5   -- this trailing comment is ignored
```

### Form 3 — `/-- --/` block doc

For longer, multi-paragraph documentation. Each body line begins with ` -`.
Content is Markdown.

```luc
/--
 - Computes the dot product of two vectors.
 -
 - Returns a scalar equal to `|a| * |b| * cos(angle)`.
 - Result is **zero** if the vectors are perpendicular.
 -
 - ## Example
 - ```luc
 - let d float = a.dot(b)
 - ```
--/
pub impl Vec2 {
    dot (other Vec2) float = { return x*other.x + y*other.y }
}
```

### Doc inside impl — attaches to the next method

```luc
pub impl Vec2 {
    -- Returns the Euclidean length of this vector.
    length () float = { return (x*x + y*y) -> sqrt }

    -- Returns the dot product with another vector.
    dot (other Vec2) float = { return x*other.x + y*other.y }
}
```

## 26. Vulkan Example

A realistic multi-file snippet: fixed-width types, `extern` FFI, struct
layouts with explicit padding, `enum` flag bits, pipelines, and the
two-layer visibility model.

```luc
-- renderer/types.luc
package renderer

pub type VkResult   = uint32
pub type VkInstance = uint64
pub type VkDevice   = uint64

pub imt VK_SUCCESS VkResult = 0x00000000
pub imt VK_ERROR   VkResult = 0x00000001

pub enum ShaderStage {
    Vertex   = 0x01
    Fragment = 0x02
    Compute  = 0x04
    Geometry = 0x08
}

-- Vertex layout — field order matches Vulkan std430
pub struct Vertex {
    pos   [3]float    -- vec3 at offset 0  (12 bytes)
    color [4]float    -- vec4 at offset 12 (16 bytes)
    uv    [2]float    -- vec2 at offset 28 (8 bytes)
}

pub struct PushConstants {
    modelMatrix [16]float
    viewProj    [16]float
    time        float
    _pad        [3]float    -- explicit padding to satisfy std430 alignment
}
```

```luc
-- renderer/instance.luc
package renderer

extern let vkCreateInstance (
    pCreateInfo @VkInstanceCreateInfo
    pAllocator  @VkAllocationCallbacks
    pInstance   @VkInstance
) VkResult

extern let vkDestroyInstance (
    instance   VkInstance
    pAllocator @VkAllocationCallbacks
)

/--
 - Creates a Vulkan instance with the given application name.
 - Returns the instance handle, or nil on failure.
--/
pub let createInstance (appName string) VkInstance? = {
    let info VkInstanceCreateInfo = VkInstanceCreateInfo { pApplicationName = appName }
    let handle VkInstance = 0
    let result VkResult = vkCreateInstance(&info, nil, &handle)
    if result != VK_SUCCESS {
        io.printl("vkCreateInstance failed: " + string(result))
        return nil
    }
    return handle
}
```

```luc
-- renderer/frame.luc
package renderer

let uploadVertices   (verts []Vertex) uint64 = { ... }
let buildCommands    (buf uint64) []byte     = { ... }
let submitAndPresent (cmds []byte)           = { ... }

-- The whole frame as a single pipeline
let renderFrame (vertices []Vertex) = {
    uploadVertices(vertices)
    -> buildCommands
    -> submitAndPresent
}

-- DOD-style parallel transform before upload
let transformAndRender (vertices []Vertex) = {
    parallel for v in vertices {
        v.pos = v.pos -> transformPos
        v.uv  = v.uv  -> wrapUV
    }
    renderFrame(vertices)
}
```

```luc
-- renderer/module.luc  (curated external API)
package renderer

module renderer {
    use renderer.types
    use renderer.instance
    use renderer.frame
}
```

```luc
-- app/main.luc
package app

use renderer

pub let main () = {
    let instance VkInstance = renderer.createInstance("luc-demo") ?? {
        io.printl("failed to init Vulkan")
        return
    }

    let verts []Vertex = [
        Vertex { pos = [0.0, -0.5, 0.0]  color = [1.0, 0.0, 0.0, 1.0]  uv = [0.5, 0.0] }
        Vertex { pos = [-0.5, 0.5, 0.0]  color = [0.0, 1.0, 0.0, 1.0]  uv = [0.0, 1.0] }
        Vertex { pos = [0.5,  0.5, 0.0]  color = [0.0, 0.0, 1.0, 1.0]  uv = [1.0, 1.0] }
    ]

    renderer.transformAndRender(verts)

    vkDestroyInstance(instance, nil)
}
```

---

*Examples are aligned with the grammar as finalised on 2026-03-14.
Update this file alongside `LUC_GRAMMAR.md` as the language evolves.*