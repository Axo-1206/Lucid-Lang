# Lucid — Grammar Reference

> Lucid is the successor to Luc. It keeps Luc's core ideas — primitive types,
> function types, currying, generics, pipeline, composition, result-based error
> handling, and foreign-function communication — while removing `impl`, `from`,
> traits, and method dispatch entirely. All behavior is expressed as plain
> functions. The language is designed to be visual-first: every construct maps
> cleanly to a visual block or graph node.

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

STRING_LIT  = '"' { STRING_CHAR } '"'
            | '`' { ANY_CHAR } '`'      (* raw string — no escape processing *)

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
decl        struct      enum        trait       use
as          if          else        switch      case        default
return      break       continue    while       for         in
do          await       and         or          not         true
false       nil         err
```

> [!NOTE]
> `let` and `const` are **not** keywords in Lucid. Mutability and immutability
> are expressed as qualifiers: `~[mut]` and `~[const]`. Every declaration uses
> the single keyword `decl`.
>
> `pub`, `export`, `extern` and `link` are **not** keywords. Visibility and
> linkage are expressed as attributes: `@[export]`, `@[foreign("C")]`,
> `@[link("libname")]`.
>
> `impl`, `from`, `trait`, `type`, `package` do not exist in Lucid. Package
> identity is determined by the file path relative to the package root declared
> in the build manifest — no per-file declaration is required.
>
> **`type` (type alias) was deliberately considered and rejected — see below.**
>
> `await` suspends the current async function until the awaited call resolves.
> It is only valid inside a `~[async]`-qualified function body. See
> **Async / Await**.

> [!WARNING]
> **Type alias (`type X = Y`) was deliberately rejected — do not re-propose it
> without solving the problem below first.**
>
> The blocking problem is not weak motivation (a transparent primitive alias
> like `type cm = int` was also considered and found to add no compiler-enforced
> safety — a good variable name or a doc comment communicates the same thing).
> The blocking problem is **structural**: a type alias that names a function
> type breaks parsing itself, not just readability.
>
> `func_decl` only ever introduces parameter names by writing `(name type)`
> directly in a literal `param_group` — see **Function Declaration**. Consider:
>
> ```lucid
> type MagicFunction = (a int) -> int
>
> decl doSomething MagicFunction = { return a + 5 }
> ```
>
> This single line is **structurally ambiguous to the parser**, not just to a
> human reader. With no alias resolved yet, `decl doSomething MagicFunction =
> { ... }` matches `var_decl = 'decl' IDENTIFIER { decl_qualifier } type [ '='
> expr ]` exactly as well as it would need to match a function declaration —
> the parser cannot know `MagicFunction` is a function type, and therefore
> cannot know `a` is a parameter name introduced by it, without first
> resolving what the alias means. That resolution is a type-checking concern,
> but the decision of *which grammar production this even is* — `var_decl`
> vs. a function declaration — has to happen during parsing, before type
> checking exists. This inverts the normal parse-then-typecheck pipeline and
> was judged not worth the complexity it would force onto the rest of the
> compiler.
>
> A future proposal must solve this structural problem — not just argue the
> feature is useful — before being reconsidered. One direction that avoids it
> entirely: restrict any alias to types that can never appear bare on the
> left of `decl IDENTIFIER <alias> = expr` in a way that could be confused
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

/--
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
decl normalize ~[const] (v Vec2) -> Vec2 = { ... }    -- stacked attaches

decl maxVertices ~[const] int = 65536   -- Vulkan hard limit   -- trailing attaches

/--
 - Computes the dot product of two vectors.
 -
 - Returns `|a| * |b| * cos(angle)`.
--/
decl dot ~[const] (other Vec2) -> float = { ... }    -- block attaches
```

---

## Separators `,` and `;`

Commas `,` and semicolons `;` are **optional** — Lucid uses newlines and block
structure to delimit constructs. The parser accepts them but never requires them.

---

## Top-Level Structure

```ebnf
program         = { top_level_item }

top_level_item  = { attribute_list } { decl_qualifier } top_level_decl
                | use_decl

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
use_decl    = 'use' use_path [ 'as' IDENTIFIER ]

use_path    = IDENTIFIER { '.' IDENTIFIER }
```

```lucid
use std.io
use std.math as math
use graphics.gl as gl
```

### Exported Name Immutability

`@[export]` makes a name visible outside its module, but the binding belongs to
the module that declared it. From outside the module, exported names are always
**read-only** — they cannot be reassigned regardless of whether the declaration
is `~[mut]` or `~[const]` internally:

```lucid
-- inside mymod.lucid
@[export] decl counter ~[mut] int = 0    -- mutable inside the module
@[export] decl PI ~[const] float = 3.14  -- immutable everywhere

-- inside another module
use mymod

mymod:counter = 1    -- ERROR: exported names are read-only from outside the module
mymod:PI      = 3.0  -- ERROR: same rule
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
use std.math as math
use std.io   as io

-- reading an exported value
decl tau ~[const] float = math:TAU

-- calling an exported function
decl result ~[const] float = math:sqrt(2.0)

-- piping through an exported function
decl normalized ~[const] float = 3.14 |> math:clamp

-- chaining: result of a call is a value, access its fields with .
decl len ~[const] int = math:split("a,b,c"):length   -- ERROR: : is only for module access
decl parts ~[const] [*]string = math:split("a,b,c")
decl len   ~[const] int       = parts.length          -- OK: . for struct field
```

**`:` vs `.` — the rule:**

| Syntax        | Left-hand side | Assignable  | Example         |
| ------------- | -------------- | ----------- | --------------- |
| `module:mem`  | module name    | never       | `math:sqrt(x)`  |
| `value.field` | struct value   | if `~[mut]` | `player.health` |

The compiler rejects `module:member = ...` at the assignment site regardless
of the member's internal mutability qualifier. This is enforced syntactically —
`:` never produces an l-value.

#### Depth of the Read-Only Guarantee

"Always read-only" applies to **everything reachable through `:`**, not just
the immediate result — a struct obtained via `:` is treated as `~[const]` at
every field depth, regardless of how its fields are individually qualified
internally. This distinguishes two cases that look similar but are not:

```lucid
-- the module exports a FUNCTION that returns a new struct each call
@[export] decl makeUser ~[const] () -> User = { return User { id = 0  name = ""  email = "" } }

decl u ~[const] User = mymod:makeUser()
u.name = "alice"           -- OK: u is a fresh value the caller owns outright;
                            -- mymod:makeUser() is the module_expr, already
                            -- fully resolved to a plain User before '.name'
                            -- is ever reached

-- the module exports a STRUCT VALUE directly (a package-level variable)
@[export] decl currentUser ~[mut] User = User { id = 1  name = "bob"  email = "" }

mymod:currentUser.name = "eve"    -- ERROR: cannot assign through a value
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

decl current ~[mut] Config = Config { threshold = 10 }   -- NOT exported

@[export] decl getThreshold ~[const] () -> int = { return current.threshold }

@[export] decl setThreshold ~[const] (v int) -> () = {
    if v < 0 { return }    -- the module can validate, log, or guard here
    current.threshold = v
}
```

```lucid
-- from outside the module
use config

decl t ~[const] int = config:getThreshold()
config:setThreshold(20)
config:current.threshold = 5    -- ERROR: current is not exported at all,
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
decl ids    [*]int    = []
decl names  [*]string = []

@[export] decl getUser ~[const] (id int)    -> User?    = { ... }   -- one full record
@[export] decl getUser ~[const] (ids [*]int) -> [*]User  = { ... }   -- many full records
@[export] decl getUser ~[const] ()           -> [*]int   = { return ids }  -- ids only, no copy of names
```

```lucid
-- from outside the module — caller picks the shape it actually needs
decl one  ~[const] User?  = users:getUser(7)
decl many ~[const] [*]User = users:getUser([7, 8, 9])
decl all  ~[const] [*]int  = users:getUser()
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
                   Raw foreign memory uses ptr<byte> instead. *)

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
                | qualified_type
                | tuple_type
                | IDENTIFIER                (* named type reference *)

qualified_type  = '~[' type_qual_item { ',' type_qual_item } ']' type
                | type '?'                  (* nullable: T or nil *)
                | type '!'                  (* fallible: T or err *)
                | type '?' '!'              (* nullable and fallible: T, nil, or err *)
                  (* qualifier applied to a type rather than a declaration *)
                  (* e.g. ~[nullable] int, ~[async] (int) -> bool *)
                  (* '?!' is the only valid order — '!?' does not parse *)
                  (* ! carries no payload — see Result Type and Error Handling *)

tuple_type      = '(' type ',' type { ',' type } ')'

array_type      = '[' array_size ']' type
array_size      = '*'       (* owned heap array — Lucid owns the memory *)
                | '_'       (* slice — borrowed view, Lucid does not own *)
                | INT_LIT   (* fixed-size stack array *)

(* Nullable element vs nullable array:
   [*]T?            — array of nullable T  (? binds to element, common)
   ~[nullable] [*]T — nullable array of T  (rare — prefer empty array [] instead) *)

ptr_type        = 'ptr' '<' type '>'        (* raw pointer — FFI use only *)

generic_type    = IDENTIFIER '<' type_arg { ',' type_arg } '>'
type_arg        = type | INT_LIT
```

---

## Trait Declaration

A trait is a pure **field contract** — a named set of fields (name and type
only) that a struct promises to contain. Traits have no methods, no behavior,
no qualifiers, and no default values. They exist solely to express structural
requirements for data polymorphism.

```ebnf
trait_decl  = 'trait' IDENTIFIER [ generic_params ] '{' { trait_field } '}'

trait_field = IDENTIFIER type
              (* name and type only — no qualifiers, no defaults, no behavior *)
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

- Trait fields declare **name and type only** — no `~[const]`/`~[mut]`, no
  default values. Qualifiers and defaults belong to the implementing struct.
- A struct implementing a trait must declare all the trait's fields at the
  **top level** with matching names and types.
- Field type mismatch is a **compile error** at the struct declaration site.
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

struct Bad : A, B {   -- ERROR: field x required as float by A and int by B
    x float           -- which one?
}

-- name conflict: same field, same type — fine, satisfied once
trait A { x float }
trait B { x float, y float }

struct Both : A, B {  -- OK: x satisfies both A and B
    x float
    y float
}
```

---

## Struct Declaration

A struct is a named holder — a collection of typed fields, each on its own
line. A struct may implement one or more traits by listing them after `:`.

```ebnf
struct_decl     = 'struct' IDENTIFIER [ generic_params ]
                  [ ':' trait_ref { ',' trait_ref } ]
                  '{' { struct_field } '}'

struct_field    = { attribute_list } IDENTIFIER { decl_qualifier } type [ '=' expr ]
                  (* name then type — Go style *)
                  (* matches var_decl's order: IDENTIFIER before decl_qualifier,
                     e.g. step ~[const] int — not ~[const] step int *)
                  (* type may carry type qualifiers: x T? or f ~[async] (int)->bool *)
                  (* optional default value *)

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
    next  ptr<Node<T>>?    -- nullable pointer to next node
}

struct Player {
    name   string
    health int    = 100
    speed  float  = 1.0
    active bool   = true
}

-- struct implementing traits
struct Entity : Vector2, Named {
    name   string     -- satisfies Named
    x      float = 0.0 -- satisfies Vector2
    y      float = 0.0 -- satisfies Vector2
    health int   = 100
}

-- field typed as a trait — accepts any struct implementing Vector2
struct PhysicsBody {
    position  Vector2   -- any struct implementing Vector2
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
decl p ~[const] Point = Point { x = 3.0, y = 4.0 }

-- omit fields that have defaults
decl origin ~[const] Point = Point {}          -- x=0.0, y=0.0 from defaults
decl shifted ~[const] Point = Point { x = 5.0 } -- x=5.0, y=0.0 from default

-- nested struct
struct Rect {
    origin Point
    width  float = 0.0
    height float = 0.0
}

decl r ~[const] Rect = Rect {
    origin = Point { x = 1.0, y = 2.0 }
    width  = 100.0
    height = 50.0
}

-- generic struct
struct Box<T> { value T }

decl b1 ~[const] Box<int>    = Box<int>    { value = 42 }
decl b2 ~[const] Box<string> = Box<string> { value = "hello" }
decl b3 ~[const] Box<Point>  = Box<Point>  { value = Point { x = 1.0, y = 2.0 } }

-- mutable struct — fields can be reassigned
decl player ~[mut] Player = Player { name = "hero" }
player.health = 80
player.speed  = 1.5

-- struct with nullable field
struct Enemy {
    name   string
    target Player?    -- nullable — may have no target
}

decl e ~[const] Enemy = Enemy { name = "goblin", target = nil }
decl e2 ~[const] Enemy = Enemy { name = "orc", target = player }
```

### Field Access

Fields are accessed with `.`:

```lucid
decl px ~[const] float = p.x         -- 3.0
decl py ~[const] float = p.y         -- 4.0
decl rw ~[const] float = r.width     -- 100.0
decl ox ~[const] float = r.origin.x  -- 1.0  (nested access)

-- nullable field — guard before access
if e2.target != nil {
    decl hp ~[const] int = e2.target.health
}
```

### Const Fields and Function-Typed Fields

A field qualified `~[const]` cannot be reassigned through `field_expr`, even
when the containing variable is itself `~[mut]`. Field-level `~[const]` is
part of the struct's own definition, not something the holder of a mutable
variable can override — `player ~[mut] Player` makes `player`'s *mutable*
fields reassignable through `player.field = ...`, but a field the struct
itself declared `~[const]` stays read-only regardless:

```lucid
struct Counter {
    step    ~[const] int   -- fixed for the lifetime of every Counter value
    total   ~[mut]   int
}

decl c ~[mut] Counter = Counter { step = 1  total = 0 }
c.total = 5       -- OK: total is ~[mut]
c.step  = 2       -- ERROR: step is ~[const] — read-only even though c is ~[mut]
```

This applies the same way to a field of **function type**. Luc introduced
`impl` partly to prevent a struct's behavior from being reassigned after
construction — Lucid has no `impl` and no methods at all (see the opening
note on removed features), so this concern only ever applies to an ordinary
field that happens to hold a function value, and `~[const]` already covers it
with no further mechanism needed:

```lucid
struct Validator {
    check ~[const] (int) -> bool   -- fixed behavior, set once at construction
}

decl positive ~[const] Validator = Validator {
    check = (n int) -> bool { return n > 0 }
}

positive.check = (n int) -> bool { return n < 0 }   -- ERROR: check is ~[const]

decl result ~[const] bool = positive.check(5)   -- OK: calling through a
                                                  -- const field is unaffected;
                                                  -- only reassignment is blocked
```

A field left `~[mut]` (the default, same as any other declaration) can be
reassigned freely, including to a different function value — useful for
genuinely swappable behavior, like a configurable callback:

```lucid
struct Logger {
    sink ~[mut] (string) -> () = (msg string) -> () { io:printl(msg) }
}

decl log ~[mut] Logger = Logger { }
log.sink = (msg string) -> () { system:writeToFile("app.log", msg) }   -- OK
```

## Enum Declaration

An enum is a set of named variants, each with a **required** integer value —
Lucid does not auto-increment omitted values, the same no-inference stance
applied everywhere else in this grammar (see **Variable Declaration**: a
type is always written, even where the answer looks unambiguous; the same
principle applies here to values, not just types). Visually: a vertical
block where each variant is a row with a name cell and a value cell.

```ebnf
enum_decl       = 'enum' IDENTIFIER [ ':' int_type ] '{' { enum_variant } '}'

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
    North      -- ERROR: value is required, even though "the next int" looks obvious
    East  = 1
}
```

---

## Function Declaration

A function declaration binds a name to a function value. Visually: a horizontal
block where each parameter group is a column, separated by `->` arrows that mark
real execution boundaries.

```ebnf
func_decl       = 'decl' IDENTIFIER [ generic_params ]
                  { decl_qualifier }
                  param_group { param_group }     (* Form 2: ()() shorthand *)
                  [ '->' return_type ]            (* omitted for void/unit *)
                  '=' func_body

                | 'decl' IDENTIFIER [ generic_params ]
                  { decl_qualifier }
                  param_group '->' func_type      (* Form 1: explicit intermediate -> *)
                  '=' func_body

param_group     = '(' [ param_list ] ')'
param_list      = param { ',' param } [ ',' variadic_param ]
                | variadic_param
param           = IDENTIFIER type               (* name then type — mutable by default *)
                | IDENTIFIER { decl_qualifier } type
                                                 (* ~[const] marks read-only parameter *)
variadic_param  = IDENTIFIER '...' type
                  (* must be the last parameter in its param_group; collects
                     zero or more trailing arguments into a [*]type array *)

return_type     = type
                | '(' type { ',' type } ')'     (* multiple return *)

func_body       = '{' { statement } '}'
                | expr                           (* single-expression body *)

func_type       = param_group { param_group } '->' return_type
                  (* Form 2 function type — matches Form 2 declaration shape *)
                | param_group '->' func_type
                  (* Form 1 function type — explicit intermediate arrow *)
                | param_group '->' return_type
                  (* single-group function type *)
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
decl sum ~[const] (nums ...int) -> int = {
    decl total ~[mut] int = 0
    for n int in nums {
        total = total + n
    }
    return total
}

sum()           -- 0
sum(1, 2, 3)    -- 6

-- variadic combined with regular parameters: variadic must come last
decl logf ~[const] (level int, fmt string, args ...string) -> () = {
    -- args is [*]string
}

-- INVALID — variadic is not the last parameter
decl bad ~[const] (nums ...int, label string) -> int = { ... }  -- ERROR

-- INVALID — variadic is not the last parameter of ITS OWN group
decl bad2 ~[const] (nums ...int, label string)(words ...string) -> int = { ... }  -- ERROR
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
decl summarize ~[const] (nums ...int)(words ...string) -> string = {
    decl total ~[mut] int = 0
    for n int in nums { total = total + n }

    decl joined ~[mut] string = ""
    for w string in words { joined = joined + w + " " }

    return stringFromInt(total) + ": " + joined
}

summarize(1, 2, 3)("a", "b")    -- nums = [1, 2, 3], words = ["a", "b"]
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
decl makeAdder ~[const] (base int) -> (n int) -> int = {
    decl adjusted ~[const] int = base * 2      -- runs at makeAdder(base)
    return (n int) -> int { return adjusted + n }
}

decl addTen ~[const] (n int) -> int = makeAdder(5)
addTen(3)   -- 13
```

### Form 2 — `()()` Shorthand

Think of Form 2 as a multi-parameter function that the compiler automatically
makes curriable. You write the function exactly as if all the arguments arrive
at once — the body is flat, all parameters are in scope, the logic reads left
to right. The only difference from a plain multi-param function is that `()()`
groups instead of `(,)` commas, which tells the compiler to make each group
independently callable.

The compiler expands Form 2 recursively and exhaustively into Form 1 before
type checking. The written body is never modified — only wrapper layers are
generated around it.

```lucid
-- as written
decl add ~[const] (a int)(b int) -> int = {
    return a + b
}

-- compiler expands to
decl add ~[const] (a int) -> (b int) -> int = {
    return (b int) -> int {
        return a + b
    }
}
```

### Currying and Partial Application

Form 2 (`()()`) is pure textual shorthand for Form 1. The compiler expands it
recursively before type checking. The body is never modified.

The core idea:
- `(a T, b U)` — both values arrive in one call
- `(a T)(b U)` — same values, arriving in two separate calls
- `-> T` — the body must return a value of type `T`
- Every `->` in a signature is a promise that something runs at that boundary

```lucid
-- Form 1: code runs between groups
decl makeProcessor ~[const] (config Config) -> (data string) -> string = {
    decl compiled ~[const] CompiledConfig = compile(config)   -- runs once at partial application
    return (data string) -> string { return apply(compiled, data) }
}

-- Mixing Forms: Form 2 groups first, Form 1 explicit return after
decl process ~[const] (a int)(b int) -> (c int) -> int = {
    decl sum ~[const] int = a + b               -- runs when process(a)(b) is called
    return (c int) -> int { return sum + c }
}

-- Form 2: flat body, compiler handles the wrapping
decl clamp ~[const] (lo int)(hi int)(v int) -> int = {
    if v < lo { return lo }
    if v > hi { return hi }
    return v
}

-- partial application
decl clamp0to100 ~[const] (v int) -> int = clamp(0)(100)
clamp0to100(42)    -- 42
clamp0to100(200)   -- 100
```

### Entry Point

```lucid
@[export, aot] decl main ~[const] () -> int = {
    return 0
}

-- with command-line arguments
-- [_]string: slice — the runtime owns the argument buffer, main gets a
-- read-only view. [*]string would be wrong here: that implies main owns a
-- heap copy of all arguments, which the runtime never hands over.
@[export, aot] decl main ~[const] (args [_]string) -> int = {
    return 0
}
```

`@[aot]` is shown explicitly above rather than omitted — see **Compilation
Mode Directives**, immediately below, for why a bare `@[export] decl main`
is shorthand for the same thing, not a third "unspecified" mode.

### Compilation Mode Directives

`@[aot]` and `@[jit]` are mutually exclusive attributes, valid only on `main`
(see **Built-in Attributes**, under **Compiler Directives**). `main` is
always compiled in exactly one of the two; `@[aot]` is the default:

```
@[aot]   -- ahead-of-time: produces a native binary at build time (default)
@[jit]   -- just-in-time:  compiles and executes at runtime
```

Both combine with `@[export]` in the same bracket — `attribute_list` is one
`@[...]` with comma-separated items, not separate brackets stacked together
(that stacking pattern is for qualifiers, e.g. `~[async] ~[const]`; it does
not apply to attributes):

```lucid
@[export, aot] decl main ~[const] () -> int = {
    return 0
}

-- equivalent to the above — @[aot] is the default when neither is written
@[export] decl main ~[const] () -> int = {
    return 0
}

-- opting into JIT instead requires writing @[jit] explicitly
@[export, jit] decl main ~[const] () -> int = {
    return 0
}
```

---

## Function Overloading

Two or more `decl` with the same name but different parameter signatures are
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
- Differing only in return type is a **compile error**.
- A generic function and a concrete overload of the same name coexist — the
  concrete overload takes priority when argument types match exactly.
- Cross-module overloads with the same name must be qualified at the call
  site: `mymod:process(42)` rather than `process(42)`.

```lucid
-- concrete overloads — same name, different parameter types
decl describe ~[const] (v int)    -> string = { return "int: "    + stringFromInt(v) }
decl describe ~[const] (v float)  -> string = { return "float: "  + stringFromFloat(v) }
decl describe ~[const] (v bool)   -> string = { return "bool: "   + stringFromBool(v) }
decl describe ~[const] (v string) -> string = { return "string: " + v }

describe(42)      -- resolves to (int) -> string
describe(3.14)    -- resolves to (float) -> string
describe(true)    -- resolves to (bool) -> string
describe("hi")    -- resolves to (string) -> string

-- generic and concrete coexist — concrete wins on exact match
decl process<T>  ~[const] (v T)   -> string = { return "generic" }
decl process     ~[const] (v int) -> string = { return "concrete int" }

process<string>("hi")   -- generic: "generic"
process<int>(42)        -- concrete wins: "concrete int"
process(42)             -- concrete wins: "concrete int"

-- return-type-only difference: compile error
decl bad ~[const] (v int) -> string = { ... }
decl bad ~[const] (v int) -> int    = { ... }
-- ERROR: overloads differ only in return type — unresolvable at call site
```


---

## Variable Declaration

Variables are declared with `decl`. Every `decl` is mutable by default — a
qualifier is not required to get that behavior. Write `~[const]` to make a
binding immutable, or `~[mut]` only where calling out mutability explicitly
aids the reader (see **Rules for Declaration Qualifiers**, under **Compiler
Directives**); omitting both is the common case, not an error.
A type is **always required** — Lucid is a compiler, not an interpreter, and
does not infer a type from an initializer expression, even where the
initializer looks unambiguous. A bare numeric literal like `0.01` does not
by itself say whether it means `float`, `double`, or `decimal` — these are
distinct types with different precision and performance characteristics, and
silently picking one would be a guess, not a fact derivable from the source.
The same principle applies uniformly, including to literals that happen to
match only one type (a string literal could only ever be `string`): the rule
is stated once, with no per-case exceptions to remember.

```ebnf
var_decl    = 'decl' IDENTIFIER { decl_qualifier } type [ '=' expr ]
```

```lucid
decl x int             = 42        -- mutable by default
decl pi ~[const] float = 3.14159   -- immutable
decl name ~[const] string = "lucid"   -- type still required, even though
                                        -- "lucid" could only ever be string

decl bad ~[const] = "lucid"   -- ERROR: type is required, even when the
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
decl magnitude<T : Vector2> ~[const] (v T) -> float = {
    return sqrt(v.x * v.x + v.y * v.y)   -- x and y accessible because T : Vector2
}

-- multiple constraints on the SAME parameter — '+' joins them
decl describeEntity<T : Vector2 + Named> ~[const] (v T) -> string = {
    return v.name + " at (" + stringFromFloat(v.x) + ", " + stringFromFloat(v.y) + ")"
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
decl distanceBetween<T : Vector2, U : Vector2> ~[const] (a T)(b U) -> float = {
    decl dx ~[const] float = a.x - b.x
    decl dy ~[const] float = a.y - b.y
    return sqrt(dx * dx + dy * dy)
}

-- works on any struct implementing Vector2
struct Point  : Vector2 { x float = 0.0  y float = 0.0 }
struct Entity : Vector2, Named { name string  x float = 0.0  y float = 0.0 }

magnitude<Point>(Point { x = 3.0, y = 4.0 })     -- OK → 5.0
magnitude<Entity>(Entity { name = "hero", x = 3.0, y = 4.0 })  -- OK → 5.0
magnitude<int>(42)   -- ERROR: int does not implement Vector2

-- T = Point, U = Entity — two DIFFERENT concrete types, each satisfying
-- Vector2 independently; distanceBetween never requires them to match
decl p ~[const] Point  = Point  { x = 0.0, y = 0.0 }
decl e ~[const] Entity = Entity { name = "hero", x = 3.0, y = 4.0 }
distanceBetween<Point, Entity>(p)(e)   -- OK → 5.0

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
decl addVectors ~[const] (v1 Vector2)(v2 Vector2) -> Vector2 = {
    return Vector2 { x = v1.x + v2.x, y = v1.y + v2.y }
}

decl e ~[const] Entity = Entity { name = "hero", x = 1.0, y = 2.0 }
decl added ~[const] Vector2 = addVectors(e)(e)   -- returns Vector2, name discarded
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
decl scale<T : Vector2> ~[const] (v ~[mut] &T)(s float) -> () = {
    v.x = v.x * s
    v.y = v.y * s
}

decl e ~[mut] Entity = Entity { name = "hero", x = 1.0, y = 2.0 }
scale<Entity>(e)(2.0)   -- e.x and e.y scaled in place through the reference,
                         -- e.name untouched
```

### Generic Functions

The legitimate uses of generic functions in Lucid are:

**Opaque pass-through — the function never inspects `T`, only passes it:**

```lucid
decl identity<T>  ~[const] (v T)      -> T      = { return v }
decl first<T>     ~[const] (items [_]T)(length int) -> T? = {
    if length == 0 { return nil }
    return items[0]    -- runtime-checked: a literal index does not prove
                         -- in-bounds against a slice of unknown length;
                         -- see Runtime Panics
}
decl swap<T>      ~[const] (a T)(b T) -> (T, T) = { return b, a }
```

**Higher-order — type-specific logic is a callback the caller provides:**

```lucid
decl map<T, U>    ~[const] (items [_]T)(f (T) -> U)           -> [*]U  = {
    decl result ~[mut] [*]U = []
    for v T in items { arr:append<U>(result)(f(v)) }
    return result
}

decl filter<T>    ~[const] (items [_]T)(pred (T) -> bool)     -> [*]T  = {
    decl result ~[mut] [*]T = []
    for v T in items { if pred(v) { arr:append<T>(result)(v) } }
    return result
}

decl fold<T, U>   ~[const] (items [_]T)(seed U)(f (U, T) -> U) -> U   = {
    decl acc ~[mut] U = seed
    for v T in items { acc = f(acc, v) }
    return acc
}

decl sort<T>      ~[const] (items [*]T)(cmp (T, T) -> int)    -> [*]T  = { ... }
```

**Call sites — explicit type arguments always required:**

```lucid
decl nums   ~[const] [*]int    = [3, 1, 4, 1, 5]
decl strs   ~[const] [*]string = ["hello", "world"]

decl doubled ~[const] [*]int    = map<int, int>(nums)((v int) -> int { return v * 2 })
decl lengths ~[const] [*]int    = map<string, int>(strs)((s string) -> int { return strLength(s) })
decl evens   ~[const] [*]int    = filter<int>(nums)((v int) -> bool { return v % 2 == 0 })
decl sum     ~[const] int       = fold<int, int>(nums)(0)((acc int, v int) -> int { return acc + v })
decl sorted  ~[const] [*]int    = sort<int>(nums)((a int, b int) -> int { return a - b })

-- with pipeline
decl result ~[const] [*]string =
    [3, 1, 4, 1, 5, 9, 2, 6]
    |> filter<int>((v int) -> bool { return v > 3 })!
    |> sort<int>((a int, b int) -> int { return a - b })!
    |> map<int, string>(stringFromInt)!
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
decl b ~[const] Box<int>         = Box<int>    { value = 42 }
decl p ~[const] Pair<int, string> = Pair<int, string> { first = 1, second = "hello" }
```

Functions that operate on generic structs receive the instantiated type:

```lucid
decl unbox<T>    ~[const] (b Box<T>)         -> T = { return b.value }
decl rebox<T, U> ~[const] (b Box<T>)(f (T) -> U) -> Box<U> = {
    return Box<U> { value = f(b.value) }
}

decl n ~[const] int    = unbox<int>(b)
decl s ~[const] Box<string> = rebox<int, string>(b)(stringFromInt)
```

---

## Compiler Directives: Attributes, Qualifiers, and Intrinsics

Attributes `@[]`, qualifiers `~[]`, and compiler intrinsics `#` provide instructions to the compiler, modify binding properties, or execute compile-time operations.

### 1. Attributes `@[]`

Attributes precede the declaration they annotate and use a bracket-list syntax so multiple items share one delimiter pair.

```ebnf
attribute_list  = '@[' attr_item { ',' attr_item } ']'

attr_item       = IDENTIFIER [ '(' attr_args ')' ]      (* built-in attribute, fixed set *)
attr_args       = attr_arg { ',' attr_arg }
attr_arg        = STRING_LIT | INT_LIT | FLOAT_LIT | BOOL_LIT | IDENTIFIER
```

#### Built-in Attributes

| Attribute              | Valid on             | Meaning                                                  |
| ---------------------- | -------------------- | -------------------------------------------------------- |
| `@[export]`            | any top-level `decl` | Visible outside this module                              |
| `@[foreign("abi")]`    | function `decl`      | Implemented in a foreign language; ABI string e.g. `"C"` |
| `@[link("name")]`      | module or `decl`     | Link against this native library                         |
| `@[deprecated("msg")]` | any `decl`           | Compiler warning at use sites                            |
| `@[inline]`            | function `decl`      | Hint to inline at call sites                             |
| `@[noinline]`          | function `decl`      | Prevent inlining                                         |
| `@[aot]`               | `main` only          | Compile ahead-of-time to native binary                   |
| `@[jit]`               | `main` only          | Compile and execute via JIT at runtime                   |

**Rules:**
- `@[foreign]` requires the function body to be empty `{}` — the implementation
  is resolved by the linker.
- `@[aot]` and `@[jit]` are mutually exclusive and only valid on `main`.
- `main` is always compiled in exactly one of these two modes. `@[aot]` is
  the default — writing `@[export] decl main` with neither attribute present
  is shorthand for `@[export, aot] decl main`, not a third, mode-less state.
  Write `@[jit]` explicitly to opt into the other mode; there is no bare
  "unspecified" compilation mode for `main`.
- Attributes are a **fixed, closed set** — there is no user-defined or
  namespaced attribute form. A struct field requirement belongs to **traits**
  (see **Trait Declaration**), not to an attribute that generates fields at
  compile time; behavior that constrains how a declaration works belongs to a
  **qualifier** (see **Possible Plan: Future Qualifiers**, below), not to an
  attribute that injects code. Earlier drafts of this grammar included a
  compile-time metaprogramming form (`attr_func_decl`, with `meta.addField` /
  `meta.addFunction` / `meta.addAttribute` builders) for user-defined
  attributes that could generate struct fields or functions. It was removed:
  it duplicated what traits already do for structural field requirements, had
  no demonstrated use case beyond that duplication, and — unlike a qualifier,
  whose effect is fully specified by a fixed, auditable rule — its behavior
  on any given declaration would depend on arbitrary compile-time code,
  making it far less predictable than either traits or qualifiers for the one
  job it would actually have done.

### 2. Qualifiers `~[]`

Qualifiers precede the declaration they annotate. They also use a bracket-list syntax.

**What unifies a qualifier, as opposed to an attribute:** a qualifier
constrains *how a declaration behaves and what is legal to do with it* —
required syntax at use sites, what operations are forbidden inside a body,
whether a binding can be reassigned, what kind of value a type may hold. An
attribute, by contrast, describes a declaration's *standing* — where it's
available, whether it's safe or current to use, where its implementation
lives — without changing how it behaves once you're allowed to use it. This
is a behavioral test, not a "does it touch the type" test: `~[const]`/`~[mut]`
constrain what's legal to do with a binding exactly the same way
`~[async]`/`~[nullable]` constrain what's legal to do with a value, even
though only the latter two are also part of the type. See **Attributes vs.
Qualifiers**, below, for the full comparison.

In the visual editor, this is also the practical distinction: a qualifier is
a small composable modifier a person drags onto a declaration node — each one
independently toggled, stacking freely in the same `~[ , , ]` list — while an
attribute is closer to a label or annotation attached to the node, carrying
information about it rather than changing its behavior.

```ebnf
decl_qualifier  = '~[' decl_qual_item { ',' decl_qual_item } ']'
                  (* attaches to a decl binding — not part of the type *)

type_qualifier  = '~[' type_qual_item { ',' type_qual_item } ']' type
                  (* attaches to a type — travels with the value *)
                | type '?'
                  (* shorthand nullable — equivalent to ~[nullable] type *)

decl_qual_item  = 'const' | 'mut'

type_qual_item  = 'async' | 'parallel' | 'nullable' | 'fallible'
```

#### Built-in Qualifiers

**Declaration qualifiers** — attach to `decl`, modify the binding:

| Qualifier  | Meaning                                                       |
| ---------- | ------------------------------------------------------------- |
| *(none)*   | Mutable by default — same as `~[mut]`                         |
| `~[mut]`   | Explicitly mutable (redundant but valid for documentation)    |
| `~[const]` | Immutable — binding cannot be reassigned after initialisation |

**Type qualifiers** — part of the type, valid anywhere a type appears:

| Qualifier       | Meaning                                           |
| --------------- | ------------------------------------------------- |
| `T?`            | Nullable — shorthand for `~[nullable] T`          |
| `~[nullable] T` | Nullable — value may be `nil`                     |
| `T!`            | Fallible — shorthand for `~[fallible] T`          |
| `~[fallible] T` | Fallible — value may be `err`                     |
| `~[async] T`    | Async function type — caller must `await`         |
| `~[parallel] T` | Parallel function type — may execute concurrently |

**Rules for Declaration Qualifiers (`~[const]`, `~[mut]`):**
- Attach to a `decl` binding — they are properties of the name, not the type.
- Every `decl` defaults to `~[mut]` — mutable unless marked otherwise.
- `~[const]` and `~[mut]` are mutually exclusive.
- `~[mut]` is valid but redundant when written explicitly (the default); use it
  only for documentation emphasis.
- Declaration qualifiers are valid on `decl`, on parameters (to express
  read-only intent on a passed reference), and on struct fields (`struct_field`
  already includes `{ decl_qualifier }` — see **Const Fields and Function-Typed
  Fields**, under **Struct Declaration**, for what `~[const]` means there
  specifically).

> [!NOTE]
> Examples throughout this document follow the rule above: `~[mut]` is
> written only where the surrounding example specifically demonstrates or
> depends on mutation (the value is reassigned later in the same snippet, or
> the point being made is a const/mut contrast). Elsewhere, a bare `decl`
> with no qualifier is mutable by default and is the preferred style — do not
> read the presence or absence of `~[mut]` across this document as
> inconsistent; it is deliberate per-example emphasis, not a stricter rule
> than the one stated here.

**Rules for Type Qualifiers (`~[async]`, `~[parallel]`, `~[nullable]` / `?`, `~[fallible]` / `!`):**
- Are part of the type itself — they travel with the value through parameters,
  return types, struct fields, and function type signatures.
- `~[async]` and `~[parallel]` are only valid on function types.
- `~[nullable] T` and `T?` are identical — the compiler normalises both to the
  same internal form. Use `?` for brevity in everyday code; use `~[nullable]`
  when writing a full qualifier list.
- `~[fallible] T` and `T!` are identical — same normalisation, same brevity rule.
- `~[fallible]` attaches to whichever type immediately follows it, exactly like
  `~[nullable]`. This is what gives `!` an unambiguous, settled meaning on
  function types and array types — see **`!` on Function Types** and **`!` on
  Array Types** below.
- Stacking nullable is an error: `T??` and `~[nullable] T?` are both rejected.
  Stacking fails is the same error: `T!!` and `~[fallible] T!` are both rejected.
- `?` binds to the **element type** of an array, never to the array itself:
  `[*]T?` means "array of nullable T." To express a nullable array (rare —
  prefer an empty array instead), use `~[nullable] [*]T` explicitly.
- `!` binds to the **element type** of an array the same way, for the same
  reason — see **`!` on Array Types** below.

#### Attributes vs. Qualifiers

Both use a prefix-bracket syntax (`@[...]` and `~[...]`) and both precede a
declaration, but they answer different questions:

| Feature             | Attribute (`@[name]`)                                          | Qualifier (`~[name]`)                                                                                                        |
| ------------------- | -------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| **Answers**         | "Is this available, current, and where is it from?"            | "How does this behave, and what's legal to do with it?"                                                                      |
| **Attached to**     | Declaration (function, variable, struct, etc.)                 | Declaration binding, or a type (function type, value type)                                                                   |
| **Affects type?**   | No — metadata only                                             | Sometimes — `~[async]`/`~[nullable]`/`~[fallible]`/`~[parallel]` do; `~[const]`/`~[mut]` don't, but still constrain behavior |
| **When checked**    | Compile time, as metadata                                      | Type checking and call-site / body rules                                                                                     |
| **Examples**        | `@[export]`, `@[deprecated(...)]`, `@[foreign("C")]`           | `~[const]`, `~[async]`, `~[nullable]`, `~[parallel]`                                                                         |
| **User-definable?** | No — fixed set (no namespaced/user form; see **Rules**, above) | No — fixed set (see **Possible Plan**, below)                                                                                |

**Why the test isn't "does it affect the type":** `~[const]` doesn't change
a value's type, yet it clearly belongs with `~[async]`/`~[nullable]` rather
than with `@[export]`/`@[deprecated]` — it restricts what you're allowed to
do with the binding (reassign or not), the same category of constraint
`~[nullable]` applies to usage (narrow before use) and `~[async]` applies to
call syntax (`await` required). The dividing line is **behavioral
restriction vs. descriptive metadata**, not type membership.

**Why `@[export]` isn't a qualifier:** it says where a name is visible, not
how it behaves once you're allowed to see it — calling an exported function
looks identical to calling a non-exported one from inside the same module.

**Why `@[deprecated(...)]` isn't a qualifier:** it's a warning about
*currency*, not a behavioral restriction — a deprecated function still does
exactly what it always did; nothing about calling it or using its value
changes.

**Why `~[nullable]` can't be an attribute:** an attribute would only mark the
declaration as metadata — every caller would still need to remember, on
their own, to check for `nil` before use. Making it a qualifier instead puts
the obligation into the **type**, so the compiler enforces the check; a
nullable value is inert until narrowed (see **If / Else Narrowing**), the
same way a fallible value is inert until narrowed.

#### Possible Plan: Future Qualifiers

Not part of the language yet — recorded here as a direction worth
considering later, not a commitment:

- **`~[procedure]`** — no `return` statement; parameters may be mutated
  directly rather than only through `&T`. A shape for functions whose entire
  purpose is a side effect on their inputs.
- **`~[pure]`** — no parameter in any param group may be mutated, and no
  side effects (no writes to captured outer state, no I/O) are permitted
  inside the body. The inverse of `~[procedure]`.

Both would slot into the same `type_qual_item` family as `~[async]` and
`~[parallel]` — they restrict what's legal inside a function body and what
shape its parameters take, the same category of constraint, just not yet
specified to the same level of detail those two qualifiers already have (see
**Body Restrictions**, under **Parallel**, for the level of precision a new
qualifier would need before being added for real).

**User-defined qualifiers are deliberately not planned for now.** Unlike a
user-defined function or struct, a new qualifier would need its own
rule-checking logic in the compiler and its own interaction rules with every
*other* qualifier already in the fixed set (does it compose with `~[async]`?
with `~[nullable]`?) — a much larger surface than ordinary user code, and
one that's easy to get wrong in ways that are hard to predict in advance.
This may be reconsidered later; for now the qualifier set stays closed, the
same as attributes.

### 3. Compiler Intrinsics `#`

Intrinsic calls appear in expression position and are prefixed with `#`. Unlike attributes, intrinsics can take runtime expressions and types as arguments.

```ebnf
intrinsic_call  = '#' IDENTIFIER '(' [ intrinsic_arg_list ] ')'

intrinsic_arg_list = intrinsic_arg { ',' intrinsic_arg }

intrinsic_arg   = expr
                | type
```

#### Compile-Time Type Queries

| Intrinsic     | Returns  | Notes                                              |
| ------------- | -------- | -------------------------------------------------- |
| `#sizeof(T)`  | `uint64` | Byte size of type T — compile-time constant        |
| `#alignof(T)` | `uint64` | Alignment requirement of T — compile-time constant |

```lucid
decl size  ~[const] uint64 = #sizeof(Vertex)
decl align ~[const] uint64 = #alignof(Vec2)
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

```lucid
decl hyp     ~[const] float = #sqrt(x*x + y*y)
decl rounded ~[const] float = #round(value)
decl maxVal  ~[const] int   = #min(a, b)
```

#### Bit Manipulation (Integer Types Only)

| Intrinsic      | Args    | Returns | Notes                           |
| -------------- | ------- | ------- | ------------------------------- |
| `#clz(x)`      | integer | same    | Count leading zero bits         |
| `#ctz(x)`      | integer | same    | Count trailing zero bits        |
| `#popcount(x)` | integer | same    | Count set (1) bits              |
| `#bswap(x)`    | integer | same    | Reverse byte order (endianness) |

```lucid
decl leading  ~[const] uint32 = #clz(flags)
decl trailing ~[const] uint32 = #ctz(flags)
decl bits     ~[const] uint32 = #popcount(mask)
decl swapped  ~[const] uint32 = #bswap(networkOrder)
```

#### Memory Operations

| Intrinsic                 | Args               | Returns | Notes                       |
| ------------------------- | ------------------ | ------- | --------------------------- |
| `#memcpy(dst, src, len)`  | ptr, ptr, uint64   | void    | Copy bytes, no overlap      |
| `#memmove(dst, src, len)` | ptr, ptr, uint64   | void    | Copy bytes, handles overlap |
| `#memset(dst, val, len)`  | ptr, ubyte, uint64 | void    | Fill bytes with value       |

All memory intrinsics operate on raw pointers (`ptr<T>`) and are only valid inside `@[foreign("C")]`-decorated functions or other intrinsic calls.

```lucid
#memcpy(dest, src, #sizeof(Buffer))
#memset(ptr, 0, size)
```

#### Pointer Operations

| Intrinsic            | Args               | Returns  | Notes                                 |
| -------------------- | ------------------ | -------- | ------------------------------------- |
| `#toRef(ptr)`        | `ptr<T>`           | `&T`     | Assert valid, cross to safe reference |
| `#toPtr(ref)`        | `&T`               | `ptr<T>` | Convert reference to raw pointer      |
| `#ptrOffset(ptr, n)` | `ptr<T>`, int      | `ptr<T>` | Pointer arithmetic (element offset)   |
| `#ptrDiff(p1, p2)`   | `ptr<T>`, `ptr<T>` | `int64`  | Distance between pointers in elements |

These intrinsics are the only way to cross the sealed conduit boundary or perform pointer arithmetic.

```lucid
decl buf  ~[const] ptr<uint8> = malloc(1024)
decl ref  ~[mut] &uint8       = #toRef(buf)
ref = 0xFF

decl next     ~[const] ptr<uint8>     = #ptrOffset(buf, 1)
decl distance ~[const] int64          = #ptrDiff(next, buf)
```

#### Unsafe / Bit Reinterpretation

| Intrinsic        | Args        | Returns | Notes                                             |
| ---------------- | ----------- | ------- | ------------------------------------------------- |
| `#bitcast(T, x)` | type, value | `T`     | Reinterpret bits of x as type T; sizes must match |

Valid only inside `@[foreign("C")]`-decorated functions or when the compiler flag `--unsafe` is enabled.

```lucid
decl bits ~[const] uint32  = 0x3F800000
decl f    ~[const] float32 = #bitcast(float32, bits)   -- 1.0
```

---

## Statements

```ebnf
statement       = { attribute_list } { decl_qualifier } decl_stmt
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

block           = '{' { statement } '}'

expr_stmt       = expr
assign_stmt     = expr assign_op expr

if_stmt         = 'if' expr block { 'else' 'if' expr block } [ 'else' block ]

while_stmt      = 'while' expr block

do_while_stmt   = 'do' block 'while' expr

for_stmt        = 'for' IDENTIFIER type 'in' range_iter [ '..' expr ] block
                  (* range iteration — type must be numeric (int, float,
                     etc.); trailing '..' expr is the step *)
                | 'for' IDENTIFIER type 'in' expr block
                | 'for' IDENTIFIER type ',' IDENTIFIER type 'in' expr block
                  (* collection iteration — index, value, each with its own
                     required type annotation (index is always int; value
                     must match the collection's element type) even though
                     both are already fixed by the collection's own
                     declaration — Lucid does not infer a type from context,
                     even where the answer is unambiguous *)

range_iter      = expr range_op expr
                  (* start range_op end — reuses range_expr's shape; written
                     as its own production here so the for_stmt form is easy
                     to read directly off this grammar block *)

switch_stmt     = 'switch' expr '{' { case_clause } [ default_clause ] '}'
                  (* exhaustiveness: if expr is an enum type and no
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

> [!WARNING]
> **Visibility inside blocks:** `@[export]` is **not allowed** on any local declaration — it is top-level only. The parser emits an error if it appears inside a block.
>
> **Attributes on local declarations:** Attributes (`@[inline]`, `@[deprecated]`, etc.) **are allowed** on local declarations (`struct`, `enum`, `func`, `var`). They are attached to the declaration node and may be used by the semantic pass.

### Examples of Valid Local Declarations

```lucid
decl compute ~[const] () -> int = {
    @[deprecated("use newVec")]
    struct Vec2 { x float = 0.0  y float = 0.0 }

    @[inline]
    decl add ~[const] (a int)(b int) -> int = { return a + b }

    struct Point { x int = 0.0  y int = 0.0 }

    enum Color { Red = 0  Green = 1  Blue = 2 }

    decl p ~[const] Point = Point { x = 5, y = 5 }
    return add(p.x)(p.y)
}
```

### `if` / `else`

```lucid
if x > 0 {
    log("positive")
} else if x < 0 {
    log("negative")
} else {
    log("zero")
}
```

#### If / Else Narrowing

The compiler applies **type narrowing** inside branches based on the condition.

**Standard Narrowing — Inside the Block:**

When the condition checks a nullable variable, the compiler narrows its type inside the then-branch:

```lucid
decl a ~[const] int? = getValue()

if a != nil {
    -- a is int here, not int?
    decl x ~[const] int = a + 1    -- OK
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
> -- VALID: standalone if — inverse narrowing applies
> if a == nil { return }
> -- a is non-nullable here
>
> -- INVALID: has else — inverse narrowing NOT applied after the chain
> if a == nil { return } else { log("not nil") }
> -- a is still int? here
>
> -- INVALID: chained else-if — inverse narrowing NOT applied after the chain
> if a == nil { return } else if b == nil { return }
> -- a and b are still nullable here — compiler cannot know which branch ran
> ```

The condition determines what gets narrowed and in which direction:

| Condition  | Inside block            | Rest of scope (inverse)     |
| ---------- | ----------------------- | --------------------------- |
| `a == nil` | `a` is `nil`            | `a` is non-nullable         |
| `a != nil` | `a` is non-nullable     | `a` is nullable (no change) |
| `not a`    | `a` is `nil` or `false` | `a` is non-nullable         |

```lucid
-- guard: exit on nil → rest of scope is non-nullable
if a == nil { return }       -- rest: a is int
if not a    { return }       -- rest: a is int  (if a is a boolean/nullable)

-- guard: exit on non-nil → no narrowing gained after exit
if a != nil { return }       -- rest: a is int? (unchanged)
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
> decl process ~[const] (a int?)(b string?)(c User?) -> int = {
>     if a == nil or b == nil or c == nil { return -1 }
>     -- from here: a is int, b is string, c is User
>     return a + strLength(b) + c.id
> }
> ```
>
> **Loop body guards** — skip nil elements without nesting:
>
> ```lucid
> for item int? in items {
>     if item == nil { continue }
>     -- item is int for the rest of this iteration
>     process(item)
> }
> ```
>
> **Stack multiple standalone guards instead of chaining else-if** — each guard independently narrows its variables:
>
> ```lucid
> -- WRONG: chained else-if, no inverse narrowing after chain
> if a == nil { return } else if b == nil { return }
>
> -- CORRECT: two standalone guards, both narrow independently
> if a == nil { return }
> if b == nil { return }
> -- a is int, b is string here
> ```

#### If Expression — Inline Form

```ebnf
if_expr         = 'if' expr '??' expr 'else' expr
```

`else` is **required** in expression form. Both branches must produce compatible types. `??` is the separator between condition and then-branch.

```lucid
decl grade ~[const] string = if score >= 60 ?? "pass" else "fail"

-- chained (right-associative)
decl label ~[const] string = if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive"
```

### `while` and `do`/`while`

```lucid
decl i ~[mut] int = 0
while i < 10 {
    i = i + 1
}

do {
    i = i - 1
} while i > 0
```

### `for`

`for` has two forms: **range iteration**, over a numeric `start..end` (or
`start..<end`) sequence with no backing collection, and **collection
iteration**, over an existing array. **Both forms require an explicit type
annotation on every loop variable.** Lucid does not infer a loop variable's
type from context — not from a range's literal bounds, and not from a
collection's already-known element type. This matches **Variable
Declaration**: a type is always written, even where the answer looks
unambiguous, because the compiler does not guess.

**Range iteration** — the loop variable's type must be numeric (`int`,
`float`, and so on). The end bound's inclusivity is controlled by `range_op`,
matching **Range Expressions**:

```lucid
for i int in 0..10  { io:printl(stringFromInt(i)) }    -- 0 through 10 inclusive
for i int in 0..<10 { io:printl(stringFromInt(i)) }    -- 0 through 9, end excluded
```

An optional trailing `..` *expr* sets the step. Without it, the step is `1`:

```lucid
for i int in 0..10..2 { io:printl(stringFromInt(i)) }  -- 0, 2, 4, 6, 8, 10 — step of 2
```

**Collection iteration** — every loop variable still requires its own type
annotation, even though the collection's own declaration already fixes it.
Value-only, or index and value (the index is always `int`; the value must
match the collection's element type):

```lucid
use std.array as arr

decl nums ~[const] [*]int = [1, 2, 3, 4, 5]

-- value only
for v int in nums {
    log(stringFromInt(v))
}

-- index and value
for i int, v int in nums {
    log(stringFromInt(i) + ": " + stringFromInt(v))
}
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
                | tuple_expr
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

unary_expr      = ( '-' | 'not' | '~~' ) expr

binary_expr     = expr binary_op expr
binary_op       = '+' | '-' | '*' | '/' | '%' | '^'
                | '==' | '!=' | '<' | '<=' | '>' | '>='
                | 'and' | 'or'
                | '&&' | '||' | '~^' | '<<' | '>>'

func_literal    = param_group { param_group } '->' type block
                  (* anonymous function — Form 2 *)
                | param_group '->' func_type block
                  (* anonymous function — Form 1 *)

struct_literal  = IDENTIFIER '{' { field_init } '}'
                | IDENTIFIER '<' type_arg { ',' type_arg } '>' '{' { field_init } '}'
field_init      = IDENTIFIER '=' expr

array_literal   = '[' [ expr { ',' expr } ] ']'

tuple_expr      = '(' expr ',' expr { ',' expr } ')'

generic_expr    = IDENTIFIER '<' type_arg { ',' type_arg } '>' '(' [ arg_list ] ')'
```

### Range Expressions

A range is a `start range_op end` value. It appears in three positions:
range iteration in `for` (see **`for`**), a slice bound (see **Slice
Expressions**, under **Arrays**), and a `switch` case value (see **Literal
ranges**, under **`switch`**). The two range operators differ only in
whether the end is included:

```lucid
0..10     -- inclusive: 0, 1, 2, ..., 10
0..<10    -- exclusive: 0, 1, 2, ..., 9
```

In `for` and in a slice bound, `start` and `end` may be arbitrary
expressions. In a `switch` case, both bounds must be **literals** — this
matches `case_value`'s general restriction to literals and enum variants
(see **Rejected Case Values**, under **`switch`**), since a `switch` case
dispatches on a matched value, never a computed condition:

```lucid
for i int in 0..count { ... }        -- OK: count is an arbitrary expression
decl s ~[const] [_]int = nums[lo..hi] -- OK: lo, hi are arbitrary expressions

switch score {
    case 90..100: { log("A") }        -- OK: both bounds are literals
}

switch score {
    case lo..hi: { ... }              -- ERROR: case range bounds must be literals
}
```

A range is not a standalone collection value with its own type — it only
appears in the three positions above. Writing a bare range anywhere else
(e.g. `decl r ~[const] = 0..10`) is a compile error; there is no
general-purpose range type to assign it to.

### Logical Operators

`and`, `or`, and `not` are not restricted to `bool` — they accept a value of
**any type** and coerce it to a truth value first. The result of `and`, `or`,
and `not` is always `bool`.

**Truthiness rule:**

- **Non-nullable, non-fallible type** — always truthy. The type itself
  guarantees the value can never be `nil` or `err`, so this is a
  **compile-time** fact; no runtime check is generated.
- **Nullable type** (`T?` / `~[nullable] T`) — truthy unless the value
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
decl name  ~[const] string = "alice"
decl user  ~[const] User?  = findUser(1)
decl conn  ~[const] Connection! = openConnection()

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
if has(cache, key) or expensiveLoad(key) { }       -- short-circuit
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

Integer types only. `&&` and `||` are bitwise AND/OR (not logical — those use `and`/`or` keywords). This avoids ambiguity with `&` (reference operator).

| Operator | Name                |
| -------- | ------------------- |
| `&&`     | bitwise AND         |
| `\|\|`   | bitwise OR          |
| `~^`     | bitwise XOR         |
| `~~`     | bitwise NOT (unary) |
| `<<`     | left shift          |
| `>>`     | right shift         |

```lucid
decl flags   ~[const] uint32 = 0xFF00
decl mask    ~[const] uint32 = 0x0F0F
decl result  ~[const] uint32 = flags && mask     -- 0x0F00
decl merged  ~[const] uint32 = flags || mask     -- 0xFF0F
decl inv     ~[const] uint32 = ~~flags           -- bitwise NOT
decl shifted ~[const] uint32 = 1 << 4            -- 16
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
decl result ~[const] [*]string =
    [1, 2, 3]
    |> map<int, string>(stringFromInt)!
    |> filter<string>(isNonEmpty)!
    |> map<string, string>(trim)!
```

### Argument Pack `!`

`fn(args)!` is not a function call — `!` marks an intentionally incomplete
argument list. The upstream value is injected as the **first** argument when
`|>` fires:

```lucid
decl scale ~[const] (factor float)(v float) -> float = { return v * factor }

-- without !: scale(2.0) is a complete partial application — no slot for upstream
42.0 |> scale(2.0)    -- ERROR: upstream has no parameter to fill

-- with !: upstream fills the first group
42.0 |> scale(2.0)!   -- calls scale(42.0)(2.0) → 84.0
```

### Curry Functions in Pipelines

`|>` fills exactly one parameter group. A curried function with remaining
unfilled groups is a compile error as a pipeline step. Pre-apply first:

```lucid
decl clamp ~[const] (lo int)(hi int)(v int) -> int = {
    if v < lo { return lo }
    if v > hi { return hi }
    return v
}

42 |> clamp                              -- ERROR: 3 groups — upstream fills (lo), (hi) and (v) unresolved
42 |> (v int) -> int { return clamp(0)(100)(v) }  -- OK: wrap in anonymous function

-- CORRECT: pre-apply to a single-group function
decl clamp0to100 ~[const] (v int) -> int = clamp(0)(100)
42 |> clamp0to100                        -- OK → 42
150 |> clamp0to100                       -- OK → 100
```

### Generic Functions in Pipelines

Generic functions must be instantiated with explicit type arguments at the
pipeline step site. An uninstantiated generic is a compile error:

```lucid
decl identity<T> ~[const] (v T) -> T = { return v }
decl map<T, U>   ~[const] (v T)(f (T) -> U) -> U = { return f(v) }

42     |> identity<int>                       -- OK → 42
42     |> identity                            -- ERROR: uninstantiated generic
42     |> map<int, string>(stringFromInt)!    -- OK → "42"
"hello" |> map<string, int>(length)!          -- OK → 5

-- chaining generic steps
decl result ~[const] string =
    42
    |> identity<int>
    |> map<int, string>(stringFromInt)!
    |> map<string, string>(trim)!
```

### Async Steps in Pipelines

When any step is `~[async]`, the entire pipeline expression becomes async and
must be awaited:

```lucid
decl fetch  ~[async] ~[const] (url string) -> string = { ... }
decl parse  ~[const]          (raw string) -> int    = { ... }
decl double ~[const]          (n int)      -> int    = { return n * 2 }

decl result ~[const] int = await (
    "https://api.example.com"
    |> fetch
    |> parse
    |> double
)
```

### Nullable and Fallible Steps

A `~[nullable]` or `~[fallible]` function is **forbidden** as a pipeline step,
for the same reason — the pipeline has no way to narrow it mid-chain. Guard
first:

```lucid
decl transform ~[nullable] ~[const] (v int) -> int = nil

42 |> transform    -- ERROR: ~[nullable] function forbidden as pipeline step

if transform != nil {
    decl result ~[const] int = transform(42)   -- OK: guarded, called directly
}

decl handler ~[const] ~[fallible] (int) -> string = lookupHandler("divide")

5 |> handler        -- ERROR: ~[fallible] function forbidden as pipeline step

if handler != err {
    decl result ~[const] string = handler(5)   -- OK: guarded, called directly
}
```

### `|>` vs `+>` — Key Difference

|                  | `\|>` pipeline                 | `+>` composition                 |
| ---------------- | ------------------------------ | -------------------------------- |
| When             | runtime — executes now         | compile time — builds a function |
| Seed             | required — a concrete value    | none                             |
| Control flow     | full access (if, switch, etc.) | none                             |
| Result           | a value                        | a new function                   |
| Types must chain | enforced per step              | strictly enforced                |

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
decl f ~[const] (a int)    -> string = { ... }
decl g ~[const] (s string) -> bool   = { ... }

decl h   ~[const] (a int) -> bool = f +> g    -- OK: f returns string, g takes string
decl bad ~[const]          = g +> f           -- ERROR: g returns bool, f takes int

-- chain three or more
decl process ~[const] (raw string) -> bool = validate +> transform +> render
```

### Generic Functions and `+>`

`+>` is where generic functions are most powerful. Instantiated at the
composition site, a generic function acts as a universal adapter between any
two compatible types:

```lucid
decl toString<T>  ~[const] (v T)      -> string = { ... }
decl parseFloat   ~[const] (s string) -> float  = { ... }
decl double       ~[const] (x float)  -> float  = { return x * 2.0 }

-- int → string → float → float
decl intToDoubled ~[const] (x int) -> float =
    toString<int> +> parseFloat +> double

intToDoubled(42)    -- "42" → 42.0 → 84.0
intToDoubled(10)    -- "10" → 10.0 → 20.0
```

**Generics reduce boilerplate across type combinations:**

```lucid
-- without generics: one wrapper per type combination
decl pipeInt   ~[const] (v int)   -> string = validateInt   +> intToStr   +> trim
decl pipeFloat ~[const] (v float) -> string = validateFloat +> floatToStr +> trim
decl pipeBool  ~[const] (v bool)  -> string = validateBool  +> boolToStr  +> trim

-- with generics: instantiate at composition site
decl pipeInt   ~[const] (v int)   -> string = validateInt   +> toString<int>   +> trim
decl pipeFloat ~[const] (v float) -> string = validateFloat +> toString<float> +> trim
decl pipeBool  ~[const] (v bool)  -> string = validateBool  +> toString<bool>  +> trim
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
decl clamp   ~[const] (lo int)(hi int)(v int) -> int = { ... }
decl scaleBy2 ~[const] (v int)                -> int = { return v * 2 }

decl pipeline ~[const] = clamp +> scaleBy2    -- ERROR: clamp has 3 groups (left side)

decl validate ~[const] (a int) -> string = { ... }
decl checkLen ~[const] (s string)(extra int) -> bool = { ... }

decl bad ~[const] = validate +> checkLen    -- ERROR: checkLen has 2 groups (right
                                              -- side) — even though validate's
                                              -- output type matches checkLen's
                                              -- first group's input, the composed
                                              -- function would still need a second
                                              -- argument group ('extra') with no
                                              -- clear origin

-- CORRECT: pre-apply every operand to a single group first
decl clamp0to100 ~[const] (v int) -> int = clamp(0)(100)
decl pipeline    ~[const] (v int) -> int = clamp0to100 +> scaleBy2

decl checkLenWith5 ~[const] (s string) -> bool = (s string) -> bool { return checkLen(s)(5) }
decl ok            ~[const] (a int) -> bool = validate +> checkLenWith5

pipeline(150)    -- clamp → 100, scale → 200
pipeline(50)     -- clamp → 50,  scale → 100
pipeline(-10)    -- clamp → 0,   scale → 0
```

**Async composition** — when any operand is `~[async]`, the composed function
must be declared `~[async]` and awaited at the call site:

```lucid
decl fetch<T>  ~[async] ~[const] (url string) -> T      = { ... }
decl parse     ~[const]          (raw string) -> int    = { ... }
decl format    ~[const]          (n int)      -> string = { ... }

decl fetchAndFormat ~[async] ~[const] (url string) -> string =
    fetch<string> +> parse +> format

decl result ~[const] string = await fetchAndFormat("https://api.example.com")
```

**Nullable operands are forbidden** in composition:

```lucid
decl transform ~[nullable] ~[const] (v int) -> int = nil

decl pipeline ~[const] = transform +> scaleBy2    -- ERROR: ~[nullable] operand

-- CORRECT: guard before composing
if transform != nil {
    decl pipeline ~[const] (v int) -> int = transform +> scaleBy2
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

`!` is a postfix qualifier, not an infix operator. It carries no payload —
there is no separate error type to declare. A function either succeeds with
`T` or fails with the bare `err` sentinel:

```ebnf
qualified_type  = '~[' type_qual_item { ',' type_qual_item } ']' type
                | type '?'                  (* nullable: T or nil *)
                | type '!'                  (* fallible: T or err *)
                | type '?' '!'              (* nullable and fallible: T, nil, or err *)
                  (* '?!' is the only valid order — '!?' is rejected by the parser *)
```

```lucid
int!         -- holds either int or err
string!      -- holds either string or err
User?!       -- holds either User, nil, or err — three possible states
```

`!` is shorthand for `~[fallible] T`, exactly parallel to `?` being shorthand
for `~[nullable] T` (see **Built-in Qualifiers**). Writing `!` is equivalent
to writing the qualifier explicitly — the postfix spelling is brevity, not a
distinct mechanism, and this is what settles its meaning everywhere it
appears, including the two positions that would otherwise be ambiguous as a
bare postfix mark: function types and array types.

### `!` on Function Types

`!` after a function type's return type is **forbidden** — there is no way
to write it as a bare postfix mark, and no workaround exists to make the
whole function type itself fallible:

```lucid
(src string) -> int!       -- OK: the function returns int!, ! attaches to
                            -- int, the return type — not to the function type
(src string)! -> int       -- ERROR: ! cannot attach to a parameter list
~[fallible] (src string) -> int    -- OK: the function VALUE itself may be err,
                                  -- e.g. a lookup that may fail to produce
                                  -- a callable function at all
```

The two are genuinely different things, and `~[fallible]` keeps them
syntactically distinct instead of relying on a binding-rule footnote:

- `(src string) -> int!` — the **function always exists and is callable**;
  *calling* it may fail. This is the common case — almost every fallible
  operation in this document is shaped this way.
- `~[fallible] (src string) -> int` — the **function value itself** may be
  `err` before it is ever called — e.g. a registry lookup that returns a
  callback or `err` if no handler is registered. This is rare. Narrow it like
  any other fallible value before calling it:

```lucid
decl handler ~[const] ~[fallible] (int) -> string = lookupHandler("divide")
if handler == err { return err }
-- handler is (int) -> string here, callable
decl result ~[const] string = handler(5)
```

### `!` on Array Types

`!` binds to the **element type** of an array, never to the array itself —
the same rule already established for `?` (see **Built-in Qualifiers**):

```lucid
[*]int!                  -- array of fallible int — each element is independently
                          -- int!, narrowed individually when read
~[fallible] [*]int       -- the array itself may be err as a whole — rare, the
                          -- same relationship ~[nullable] [*]T has to [*]T?
```

As with `~[nullable]` arrays, prefer an empty array (`[]`) over
`~[fallible] [*]T` where possible — a whole-array failure is rarely the most
natural shape; an empty result or a per-element `!` usually models the
situation more precisely:

```lucid
decl items ~[const] [*]int!            = fetchAll()    -- each element independently fallible
decl whole ~[const] ~[fallible] [*]int = fetchAll()    -- rare: the whole fetch either
                                                          -- produces an array or fails
```

### `T?!` — Combined Nullable and Fallible

`T?!` is the single canonical spelling for "may be absent, may have failed."
The reverse order, `T!?`, does not parse — there is exactly one way to write
this combination, so source code never has to choose between equivalent
forms:

```lucid
decl x ~[const] User?! = riskyLookup()   -- OK: x holds User, nil, or err
decl y ~[const] User!? = riskyLookup()   -- ERROR: '!?' is not valid order, use '?!'
```

A `T?!` value is a genuine three-state value. Narrowing must rule out both
sentinels before the plain `T` is usable.

### Narrowing a Fallible Value

`err` narrows exactly like `nil` — using the same `if`/standalone-guard
machinery already defined for nullable types (see **If / Else Narrowing**).
No new control-flow construct is introduced.

**Standard narrowing — inside the block:**

```lucid
decl x ~[const] int! = riskyOp()

if x != err {
    -- x is int here, not int!
    decl total ~[const] int = x + 1    -- OK
}
-- x is still int! here
```

**Inverse narrowing — standalone guard with an exit:**

```lucid
decl process ~[const] (id int) -> int = {
    decl x ~[const] int! = riskyOp(id)
    if x == err { return -1 }
    -- x is int here for the rest of the function
    return x + 1
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
decl process ~[const] (id int) -> int = {
    decl x ~[const] User?! = riskyLookup(id)
    if x == nil or x == err { return -1 }
    -- x is User here — neither nil nor err remain possible
    return x.id
}
```

Stacked standalone guards work identically to the existing nullable
convention — prefer them over chained `else if` for the same soundness
reasons already established:

```lucid
-- CORRECT: standalone guards, each narrows independently
decl a ~[const] int?! = riskyOp()
decl b ~[const] string?! = riskyOp2()
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
decl x ~[const] int! = riskyOp()

x + 1                    -- ERROR: cannot apply '+' to un-narrowed int!
decl y ~[const] = x      -- ERROR: cannot assign un-narrowed int!
io:printl(x)              -- ERROR: cannot pass un-narrowed int! as argument
x.field                   -- ERROR: cannot access field on un-narrowed int!
x:method()                 -- ERROR: cannot call method on un-narrowed int!
x |> double                -- ERROR: cannot pipe un-narrowed int!
x == 5                     -- ERROR: cannot compare un-narrowed int! to int
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
decl x ~[const] int = (10 / d) ?? {
    system:logError("division failed")
    return -1
}
-- x is int — the block's last expression/return is plain int

-- block re-raises: result stays int!, not int
decl y ~[const] int! = (10 / d) ?? {
    decl retryDivisor ~[const] int = recoverDivisor(d)
    if retryDivisor == 0 { return err }    -- still no valid divisor — re-raise

    decl retried ~[const] int! = 10 / retryDivisor    -- still runtime-checked
    if retried == err { return err }
    return retried
}
-- y is int! here — caller of y still must narrow it; the failure was not
-- silently discarded, only retried
```

```lucid
-- nullable
decl a ~[const] int? = nil
decl b ~[const] int  = a ?? 0

-- fallible
decl c ~[const] int! = riskyOp()
decl d ~[const] int  = c ?? 0    -- err discarded, d = 0

-- nullable and fallible together
decl e ~[const] User?! = riskyLookup()
decl f ~[const] User   = e ?? User { id = 0  name = "guest"  email = "" }

-- never triggers: lhs is plain int
decl g ~[const] int = getValue()
decl h ~[const] int = g ?? 0    -- always g

-- block form: multiple statements, then a value
decl i ~[const] int = riskyOp() ?? {
    system:logError("riskyOp failed, using default")
    return -1
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
sentinel was this" the way a language with several `catch` clauses per
exception type can dispatch on the exception's type. There is no dedicated
multi-branch syntax for `??` — only `expr '??' expr` and `expr '??' block`
exist (see **`??` Fallback**).

A tempting but mistaken way to reach for per-sentinel handling is writing two
blocks back-to-back, expecting the first to handle `nil` and the second to
handle `err`:

```lucid
-- MISLEADING — does not mean "first block for nil, second for err":
decl x ~[const] int = riskyOp()
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
not "which sentinel was it." Give the intermediate result its own `decl` with
an explicit type, so it's visible at a glance whether the next `??` is live
or dead — this is exactly the check the MISLEADING example above skipped:

```lucid
decl x ~[const] int! = riskyOp()

decl step1 ~[const] int! = x ?? {
    decl retried ~[const] int! = retryOp()
    if retried == err { return err }
    return retried
}
-- step1 is explicitly int! — the block re-raised, so it did NOT fully
-- resolve x. This makes the next ?? visibly live, not dead:
decl step2 ~[const] int = step1 ?? -1    -- fully resolves whatever remains
```

Avoid writing this as one inline chain (`x ?? { ... } ?? -1`) even though it
is grammatically legal — without the intermediate `decl step1 ~[const] int!`
spelled out, a reader has to mentally execute the first block to know whether
the second `??` is live or dead, which is precisely the trap the MISLEADING
example above demonstrates. Prefer giving a chained intermediate its own
typed `decl`.

> [!NOTE]
> Inline chaining is a style choice, not a forbidden construct. The grammar
> does not special-case `expr '??' block` to reject a further `??` after it —
> doing so would not close the underlying problem anyway, since the identical
> readability trap exists when chaining through an ordinary function call
> instead of a block (`riskyOp() ?? fallbackOp() ?? -1` has the same
> live-or-dead ambiguity, and there is no clean syntactic line between "a
> call that happens to return a fallible type" and "a block"). The
> intermediate-`decl` convention above is a recommendation, not a rule the
> compiler enforces.

The block after `??` is an ordinary `block` — it can contain any statement,
including `if` or `switch`, exactly like a function body. This is not the
same as branching on which sentinel triggered the `??`: the `??` itself still
only ever knew "unresolved, run this block once" — the `switch` inside is
dispatching on some other value the block computes, not on `nil` vs `err`:

```lucid
-- (assume the following lives inside a function body, as with all
-- 'return'-containing snippets in this section)
decl recoveryCode ~[const] (d int) -> int = { ... }

decl d ~[const] int = getDivisor()

decl result ~[const] int = (10 / d) ?? {
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
decl handleAbsent ~[const] (lookup User?!) -> int = {
    system:logError("value was absent")
    return -1
}

decl handleFailure ~[const] (lookup User?!) -> int = {
    system:logError("operation failed")
    return -2
}

decl x ~[const] User?! = riskyLookup()

decl result ~[const] int =
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
decl handleAbsentCode  ~[const] () -> int = { system:logError("absent")  return -1 }
decl handleFailureCode ~[const] () -> int = { system:logError("failed")  return -2 }

decl code ~[const] int?! = riskyParse()

decl result ~[mut] int = 0
switch code {
    case nil: { result = handleAbsentCode() }
    case err: { result = handleFailureCode() }
    default:  { result = 0    -- see note below on narrowing in 'default' }
}
```

> [!NOTE]
> `switch` is a statement, not an expression (see **Statements**) — it cannot
> appear on the right-hand side of `decl`. Declare the target as `~[mut]`
> first and assign inside each arm, as above.
>
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

- If the compiler can **prove** the operation always succeeds — dividing by a
  nonzero literal, or indexing a fixed-size array (`[N]T`) with a literal
  index that is provably less than `N` — the expression types as plain `T`
  and no check exists at runtime. A literal index into a slice (`[_]T`) or
  dynamic array (`[*]T`) does **not** qualify, since their length is not part
  of the type and cannot be proven at compile time.
- If the compiler **cannot prove** this (e.g. dividing by a variable, indexing
  with a runtime-computed index, or indexing a slice/dynamic array at all),
  the operation is checked at runtime. Left unhandled, a failing check
  **panics**: the program terminates immediately with a diagnostic. Attaching
  `??` to the expression converts the panic into ordinary control flow
  instead — the fallback runs and the program continues.

```lucid
decl a ~[const] int = 10 / 2          -- OK: divisor is a nonzero literal, no check
decl b ~[const] int = 10 / d          -- d is a variable divisor — runtime-checked

decl c ~[const] int = 10 / d ?? -1    -- checked; panic converted to -1 on failure
decl e ~[const] int = 10 / d          -- checked; PANICS at runtime if d == 0
```

```lucid
decl items ~[const] [*]int = [1, 2, 3]
decl x ~[const] int = items[i]              -- i is runtime-computed — checked
decl y ~[const] int = items[i] ?? 0         -- checked; panic converted to 0
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
decl divide ~[const] (a int, b int) -> int! = {
    if b == 0 {
        return err
    }
    return a / b
}
```

### Propagation

A function that calls a fallible function and wants to fail the same way
narrows the result explicitly with a guard, then returns `err` itself — there
is no implicit pass-through. A fallible value may not be returned directly
without narrowing first, even when the caller's own return type matches
exactly:

```lucid
decl fetch ~[const] (url string) -> string! = { ... }

decl process ~[const] (url string) -> string! = {
    decl raw ~[const] string! = fetch(url)
    if raw == err { return err }
    -- raw is string here
    return raw
}
```

```lucid
-- INVALID: returning an un-narrowed fallible value, even with a matching
-- signature, is forbidden — the compiler cannot tell this apart from
-- forgetting to handle the failure
decl badProcess ~[const] (url string) -> string! = {
    decl raw ~[const] string! = fetch(url)
    return raw    -- ERROR: cannot return un-narrowed string!
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
mechanism (e.g. a `~[mut]` out-parameter, or a wrapping struct holding both a
status and a value). This keeps the type system simple: `!` only ever answers
"did this fail," and any richer detail is modeled with the same struct/enum
tools used everywhere else in the language, not folded into the fallible-type
syntax itself:

```lucid
enum FetchError {
    Network = 0
    Parse   = 1
    Timeout = 2
}

decl fetch ~[const] (url string)(lastError ~[mut] FetchError?) -> string! = {
    if not reachable(url) {
        lastError = FetchError.Network
        return err
    }
    return readBody(url)
}
```

### Error Handling in Pipelines

A fallible value cannot be a pipeline step directly — every step must be a
function, and narrowing is a statement, not an expression that fits inside a
pipeline. To narrow inside a pipeline, use an anonymous function as the final
step:

```lucid
decl result ~[const] string = dbFindUser(id)
    |> formatUser
    |> (v string!) -> string {
        if v == err { return "unnamed" }
        return v
    }
```

`??` can also appear at the end of a pipeline directly when the failure does
not need to be distinguished from absence:

```lucid
decl result ~[const] string = fetchData(url)
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

decl dbFindUser ~[const] (id int)(lastError ~[mut] DbError?) -> User?! = {
    if id < 0 {
        lastError = DbError.NotFound
        return err
    }
    return db:query(id)    -- returns User?, nil if not found
}

decl formatUser ~[const] (user User) -> string = {
    if user.name == "" { return "user has no name" }
    return user.name + " <" + user.email + ">"
}

decl getFormattedUser ~[const] (id int) -> string = {
    decl lastError ~[mut] DbError? = nil
    decl found ~[const] User?! = dbFindUser(id)(lastError)

    if found == err {
        system:logError("lookup failed: " + string(lastError))
        return "guest"
    }
    -- found is User? here — err ruled out, nil still possible

    decl user ~[const] User = found ?? User { id = 0  name = "guest"  email = "" }

    return user |> formatUser
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
decl owned  ~[const] [*]int    = [1, 2, 3]      -- heap array, Lucid owns memory
decl view   ~[const] [_]int    = owned           -- slice — borrows from owned
decl fixed  ~[const] [3]float  = [1.0, 2.0, 3.0] -- stack array, size fixed at compile time
```

**Nullable element vs nullable array:**

```lucid
[*]int?             -- owned array of nullable int   (? binds to element — common)
~[nullable] [*]int  -- nullable array of int         (rare — prefer [] over nil)
```

**Fallible element vs fallible array** — same binding rule, see **`!` on
Array Types**:

```lucid
[*]int!              -- owned array of fallible int  (! binds to element — common)
~[fallible] [*]int       -- fallible array of int        (rare — prefer [] over err)
```

### Array Literals

```lucid
decl empty  ~[const] [*]int    = []
decl nums   ~[const] [*]int    = [1, 2, 3, 4, 5]
decl matrix ~[const] [3][3]float = [
    [1.0, 0.0, 0.0],
    [0.0, 1.0, 0.0],
    [0.0, 0.0, 1.0]
]
```

### Element Access and Index

```lucid
decl first  ~[const] int = nums[0]
decl last   ~[const] int = nums[4]

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
use std.array as arr

decl nums  ~[const] [*]int = [3, 1, 4, 1, 5, 9, 2, 6]

-- sorting — user provides the comparison callback
decl sorted ~[const] [*]int = arr:sort<int>(nums)(
    (a int, b int) -> int { return a - b }   -- ascending
)

-- mapping — user provides the transform callback
decl doubled ~[const] [*]int = arr:map<int, int>(nums)(
    (v int) -> int { return v * 2 }
)

-- filtering — user provides the predicate callback
decl evens ~[const] [*]int = arr:filter<int>(nums)(
    (v int) -> bool { return v % 2 == 0 }
)

-- reducing — user provides the accumulator callback
decl sum ~[const] int = arr:reduce<int, int>(nums)(0)(
    (acc int, v int) -> int { return acc + v }
)

-- searching — user provides the predicate
decl found ~[const] int? = arr:find<int>(nums)(
    (v int) -> bool { return v > 4 }
)
```

Since the std array library functions are plain curried functions, they compose
naturally with pipeline and composition:

```lucid
use std.array as arr

decl result ~[const] [*]string =
    [3, 1, 4, 1, 5, 9, 2, 6]
    |> arr:filter<int>(isPositive)!
    |> arr:sort<int>((a int, b int) -> int { return a - b })!
    |> arr:map<int, string>(stringFromInt)!
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
decl nums ~[const] [*]int = [10, 20, 30, 40, 50]

decl sub    ~[const] [_]int = nums[1..3]    -- [20, 30, 40]
decl subEx  ~[const] [_]int = nums[1..<3]   -- [20, 30]
decl head   ~[const] [_]int = nums[..<2]    -- [10, 20] — start defaults to 0
decl tail   ~[const] [_]int = nums[3..]     -- [40, 50] — end defaults to length
decl all    ~[const] [_]int = nums[..]      -- [10, 20, 30, 40, 50] — whole array
```

A slice expression's bounds are runtime-checked the same way a single-element
index is — see **Runtime Panics**. A start or end that falls outside the
array, or a start greater than the end, panics unless guarded with `??`:

```lucid
decl bad ~[const] [_]int = nums[1..99]         -- PANICS: end out of bounds
decl ok  ~[const] [_]int = nums[1..99] ?? []    -- panic converted to an empty slice
```

### Slice Rules

A slice `[_]T` is a borrowed view — it does not own the underlying memory. The
backing array must outlive the slice:

```lucid
decl data  ~[const] [*]int = [1, 2, 3, 4, 5]
decl view  ~[const] [_]int = data              -- borrows from data
decl sub   ~[const] [_]int = data[1..3]        -- elements at index 1, 2, 3

-- writing through a mutable slice modifies the backing array
decl buf   [*]int = [0, 0, 0]
decl window [_]int = buf
window[0] = 42    -- buf[0] is now 42
```

### Fixed Arrays

A fixed array `[N]T` is stack-allocated. Its size must be a compile-time
integer literal:

```lucid
decl rgb   ~[const] [3]uint8  = [255, 128, 0]
decl mat4  ~[const] [16]float = [
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0
]

-- pass as slice to functions that accept [_]T
decl view ~[const] [_]float = mat4
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
    score  int        -- owned: cloned
    items  [*]string  -- owned: buffer deep-copied
}

decl a ~[const] Player = Player { score = 10, items = ["sword"] }
decl b ~[mut] Player = a
-- b.score and b.items are fully independent of a
```

### Borrowed Types — Scoped References (`&T`)

References (`&T`) allow sharing data without copying. They represent a safe borrowed view of an owned variable.

```lucid
decl a ~[const] Player = Player { … }

decl ref ~[mut] &Player = a     -- mutable shared reference
decl rc ~[const] &Player = a    -- read-only shared reference
```

| Declaration                    | Copies?     | Field mutation?       | Owner |
| ------------------------------ | ----------- | --------------------- | ----- |
| `decl b ~[mut] Player = a`     | ✅ deep copy | ✅ b's own fields      | `b`   |
| `decl ref ~[mut] &Player = a`  | ❌ shared    | ✅ visible through `a` | `a`   |
| `decl rc ~[const] &Player = a` | ❌ shared    | ❌ read-only           | `a`   |

#### The Downward Flow Rule (Reference Scoping)

To guarantee memory safety and eliminate dangling pointers without using a Garbage Collector or a complex compile-time borrow checker, references (`&T`) are strictly scoped. They are allowed to flow *downward* (into nested calls), but never *upward or sideways*.

1. **No Struct Storage:** A struct field cannot have a reference type (e.g., `field &T` is a compile error).
2. **No Array/Slice Storage:** An array or slice cannot store reference types (e.g., `[*]&T` or `[_]&T` is a compile error).
3. **No Reference Returns:** A function cannot return a reference type (e.g., `-> &T` is a compile error).

As a result, a reference (`&T`) can only exist in two places:
*   As a **function parameter** (e.g., `decl process ~[const] (p &Player)`).
*   As a **local variable alias** inside a block (e.g., `decl ref ~[mut] &Weapon = player.weapon`).

This guarantees that a reference never outlives the owned variable it points to.

#### Modeling Complex Data Structures (Trees, Graphs, Links)

Because references (`&T`) cannot be stored inside structs, building circular or linked data structures requires alternative approaches:

1. **Indices and Arenas (Recommended DOD Style):** Store integer indices into a flat array/arena instead of memory references.
   ```lucid
   struct Node {
       value int
       next  int    -- index of the next node in an array / arena
   }
   ```
2. **Raw Pointers (`ptr<T>`):** For manual memory management (e.g., building low-level systems or C integrations), use raw pointers. Raw pointers are "sealed conduits" and require explicit `#toRef` conversion to use, signaling unsafe operations. Type conversion syntax (e.g., `ptr<T>(val)` or `&T(val)`) is forbidden for raw pointers and references; the `#toRef` and `#toPtr` intrinsics must be used instead to cross the unsafe boundary.
   ```lucid
   struct Node {
       value int
       next  ptr<Node>?  -- raw pointer, nullable. Requires manual lifecycle tracking.
   }
   ```
3. **Smart Pointers (Standard Library):** For safe shared heap state, use standard library reference-counted wrappers like `Shared<T>` and `Weak<T>` (which auto-nulls when the owner is destroyed). These incur a small runtime cost.

### Function Values and Closures

Named functions are plain function pointers — no captured state. Closures (partial applications, anonymous functions capturing variables) hold a heap-allocated environment. Assigning a closure copies the reference to that environment.

---

## The Sealed Conduit Model (Raw Pointers)

Raw pointers (`ptr<T>`) are sealed conduits — carry them, pass them to foreign functions, check for nil, but never dereference directly.

### Pointer Nullability Semantics (`ptr<T>` vs `ptr<T>?`)

`?` on a raw pointer always binds to the **whole pointer type**, not to the element type:

| Type      | Meaning                                                                   | C equivalent                  |
| --------- | ------------------------------------------------------------------------- | ----------------------------- |
| `ptr<T>`  | Non-nullable — the pointer address is always valid (programmer assertion) | `T* __attribute__((nonnull))` |
| `ptr<T>?` | Nullable — `(ptr<T>)?` — the pointer itself may be nil                    | `T*` that may be `NULL`       |

```lucid
decl p ~[const] ptr<Node>  = getNode()    -- programmer asserts: address is always valid
decl q ~[const] ptr<Node>? = findNode()   -- pointer itself may be nil; nil-check required before use
```

> [!NOTE]
> Unannotated `@[foreign("C")]` pointer returns default to `ptr<T>` (non-nullable). Use `ptr<T>?` only when you know the C function may return `NULL`. The foreign declaration on the Lucid side is the sole nullability contract — the Lucid compiler does not parse C headers.

> [!CAUTION]
> **`ptr<T>` is a programmer-level contract, not a compiler-verified proof.**
>
> The type system guarantees that a non-nullable `ptr<T>` will never be *statically assigned* `nil`. It does **not** and **cannot** guarantee that the pointed-to memory remains valid at runtime. External code — foreign calls (e.g. `free`), manual pointer arithmetic via `#ptrOffset`, or aliased ownership — can release or corrupt that memory *after* the pointer was set, producing a **dangling pointer**: non-nil in value, invalid in content.
>
> A nil check (`== nil`, `!= nil`) guards against a null address. It does **not** detect a dangling pointer.
>
> **Responsibilities when using non-nullable `ptr<T>`:**
> - You assert that the pointer's target is valid for the duration you hold the pointer.
> - You own or have a clear understanding of the pointed-to memory's lifetime.
> - No other owner will free that memory while your `ptr<T>` is live.
>
> **Preferred mitigation — wrap `ptr<T>` in a cleanup struct:**
> ```lucid
> struct OwnedBuffer {
>     ptr  ptr<uint8>    -- non-nullable: asserts validity at construction
>     size uint64
> }
>
> decl disposeBuffer ~[const] (buf &OwnedBuffer) = {
>     freeBuffer(buf.ptr, buf.size)    -- lifetime ends here, predictably
> }
> ```
> For shared ownership with automatic invalidation, use the standard library's `Shared<T>` and `Weak<T>` instead.

### Allowed Operations

1. Store in a variable, struct field, or parameter
2. Pass to a `@[foreign("C")]` function
3. Nil check (`== nil`, `!= nil`)
4. Pass to pointer intrinsics (`#toRef`, `#ptrOffset`, etc.)
5. Print the address for debugging

### Forbidden Operations (Compiler Error)

- Dereferencing: `*ptr`
- Field access: `ptr.field`
- Indexing: `ptr[i]`
- Arithmetic: `ptr + 4` — use `#ptrOffset` instead
- Assignment: `*ptr = value`
- Type casting/conversion: e.g., `ptr<float>(x)` or `&float(x)` (must use `#toRef` or `#toPtr` to cross safety boundaries)

### Boundary Crossing (Intrinsics)

```lucid
#toRef(ptr)         -- ptr<T> → &T  (assert validity, cross to safe reference)
#toPtr(ref)         -- &T → ptr<T>  (convert back to raw pointer)
#ptrOffset(ptr, n)  -- pointer arithmetic, returns new ptr<T>
#ptrDiff(p1, p2)    -- distance between two pointers as int64
```

```lucid
@[foreign("C")]
decl malloc ~[const] (size uint64) -> ptr<uint8>?

decl buf ~[const] ptr<uint8>? = malloc(1024)
if buf == nil { return 1 }

decl ref ~[mut] &uint8 = #toRef(buf)           -- cross the boundary
ref = 0xFF                                     -- work with it safely

decl next ~[const] ptr<uint8>? = #ptrOffset(buf, 1)  -- pointer arithmetic
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
decl getValue ~[const] (address ptr<int>) -> int   -- returns owned int, never &int

decl addr ~[const] ptr<int> = getAddressFromSomewhere()
decl n    ~[const] int      = getValue(addr)         -- safe: int is owned, fully copied
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
decl getPlayer ~[const] (store ptr<PlayerStore>, id int) -> ptr<Player>?   -- nullable: id may not exist

decl p ~[const] ptr<Player>? = getPlayer(store, 42)
if p == nil { return }

decl ref   ~[const] &Player = #toRef(p)    -- assert validity, enter safe world
decl score ~[const] int     = ref.score    -- read fields safely through the reference
```

### How C Communicates Nullable Returns to Lucid

C has no built-in nullable type. The foreign declaration in Lucid is the **sole nullability contract** — the Lucid compiler does not parse C headers. The programmer declares the expected nullability and owns the promise.

---

## Type Conversion

There is no `from` declaration in Lucid. All conversions are explicit function
calls. Conversion functions are plain functions by convention:

```lucid
-- naming convention: targetFromSource or targetOf
decl intFromString ~[const] (s string) -> int! = { ... }
decl stringFromInt ~[const] (n int)    -> string = { ... }
decl floatFromInt  ~[const] (n int)    -> float  = { ... }

-- use
decl parsed ~[const] int! = intFromString("42")
decl n ~[const] int = parsed ?? 0
decl s ~[const] string = stringFromInt(n)
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
decl vector2Add  ~[const] (a Vector2)(b Vector2) -> Vector2 = {
    return Vector2 { x = a.x + b.x, y = a.y + b.y }
}

decl vector2Scale ~[const] (v Vector2)(s float) -> Vector2 = {
    return Vector2 { x = v.x * s, y = v.y * s }
}

decl vector2Length ~[const] (v Vector2) -> float = {
    return sqrt(v.x * v.x + v.y * v.y)
}

-- usage
decl a ~[const] Vector2 = Vector2 { x = 1.0, y = 0.0 }
decl b ~[const] Vector2 = Vector2 { x = 0.0, y = 1.0 }
decl c ~[const] Vector2 = vector2Add(a)(b)
decl len ~[const] float = vector2Length(c)
```

---

## Foreign Function Interface

Foreign functions are declared with `@[foreign("abi")]`. The body must be empty `{}` — the implementation is resolved by the linker. Multiple attributes can be combined into a single `@[...]` list.

```ebnf
foreign_decl    = '@[' foreign_attr { ',' link_attr } ']' func_decl
                  (* func body must be empty '{}' *)

foreign_attr    = 'foreign' '(' STRING_LIT ')'              (* ABI: "C", "C++", etc. *)
link_attr       = 'link' '(' STRING_LIT { ',' STRING_LIT } ')'
                  (* one or more library names or source file paths *)
```

```lucid
-- standard C library function
@[foreign("C")]
decl malloc ~[const] (size uint64) -> ptr<byte>? = {}

-- combine foreign + link in one attribute list
@[foreign("C"), link("path/to/file.c")]
decl myAdd ~[const] (a int, b int) -> int = {}

-- void return: omit the return type entirely
@[foreign("C"), link("opengl")]
decl glClear ~[const] (mask uint32) = {}

-- nullable return — C function may return NULL
@[foreign("C"), link("mylib")]
decl findNode ~[const] (id int) -> ptr<Node>? = {}

-- multiple link targets in one attribute: paths and library names can be mixed
@[foreign("C"), link("vendor/math/fast_math.c", "vendor/math/lut.c", "m")]
decl fastSin ~[const] (x float) -> float = {}
```

> [!NOTE]
> `@[link(...)]` accepts one or more comma-separated strings. Each string is
> either a library name (e.g. `"opengl"`, `"m"`) or a file path (e.g.
> `"vendor/lib/file.c"`). The two forms can be mixed freely in a single
> `link(...)` call. Paths and names are distinguished by the presence of `/`
> or a file extension — bare names are passed as `-lname` to the linker; paths
> are passed directly. Platform-specific extensions (`.dll`, `.so`, `.dylib`)
> in paths are the responsibility of the build system or conditional attributes.

> [!IMPORTANT]
> Foreign functions **must not** return `&T`. The Downward Flow Rule forbids
> reference returns from all functions, including foreign ones. Return an owned
> value or a raw `ptr<T>` and use `#toRef` on the Lucid side if needed.

---

## Operator Precedence

Highest to lowest:

| Level | Operators                   | Associativity |
| ----- | --------------------------- | ------------- |
| 8     | `+>` (composition)          | left          |
| 7     | unary `-` `not` `~`         | right         |
| 6     | `*` `/` `%`                 | left          |
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

## Async / Await

`~[async]` is a **type qualifier** — it is part of the function type, not just
a declaration hint. Two functions with identical parameter and return types but
different `~[async]` status are **different types** and are not interchangeable.

`await` suspends the current function until the awaited async call resolves. It
is only valid inside a `~[async]`-qualified function body.

```ebnf
await_expr  = 'await' expr
              (* expr must be a call to a ~[async]-qualified function *)
              (* valid only inside a ~[async] function body *)
```

```lucid
-- ~[async] is part of the function type
decl fetch ~[async] ~[const] (url string) -> string = {
    return await httpGet(url)    -- httpGet must also be ~[async]
}

-- calling a ~[async] function requires await
decl result ~[const] string = await fetch("https://api.example.com")

-- await is only valid inside a ~[async] body
decl bad ~[const] (url string) -> string = {
    return await fetch(url)    -- ERROR: not inside a ~[async] body
}
```

**`~[async]` as a type qualifier on parameters and return types:**

Because `~[async]` is part of the type, it travels through parameters and
return types. A function that accepts or returns an async function must declare
that in its signature:

```lucid
-- parameter is an async function — caller must pass a ~[async] function
decl run ~[const] (task ~[async] () -> int) -> int = {
    return await task()
}

-- return type is an async function
decl makeLoader ~[const] (prefix string) -> ~[async] (string) -> string = {
    return ~[async] (path string) -> string {
        return await fetch(prefix + path)
    }
}
```

**Rules:**

- `await expr` — `expr` must be a call to a `~[async]`-qualified function.
  Using `await` on a non-async call is a compile error.
- `await` is only valid inside a function body whose declaration carries
  `~[async]`. Using `await` in a non-async body is a compile error.
- A `~[async]` function may freely call non-async functions without `await`.
- `~[async]` and `~[parallel]` are mutually exclusive on the same type —
  a function is either async (suspendable, single-threaded cooperative) or
  parallel (concurrent), not both.
- `~[async]` is not valid on non-function types.

**Type identity:**

```lucid
-- these are three distinct types — not interchangeable
decl a ~[const] (url string) -> string         = { ... }   -- plain function
decl b ~[async] ~[const] (url string) -> string = { ... }  -- async function
decl c ~[async] ~[const] (url string) -> string? = { ... } -- async nullable

decl run (f (string) -> string) -> string = { ... }

run(a)   -- OK
run(b)   -- ERROR: ~[async] (string) -> string ≠ (string) -> string
run(c)   -- ERROR: type mismatch on both async and nullable
```

**Async and composition / pipeline:**

`~[async]` functions compose with `+>` and `|>` as long as types align. The
resulting composed function is itself `~[async]`:

```lucid
-- each step is async; the composition is async
decl pipeline ~[async] ~[const] (url string) -> string =
    fetch +> parseJson +> extractTitle

decl result ~[const] string = await pipeline("https://api.example.com")
```

---

## Parallel

`~[parallel]` marks a function type whose body may execute concurrently — one
invocation per item, with no ordering guarantee between invocations. Like
`~[async]`, it is a **type qualifier**: it is part of the function's type,
travels with it through parameters and return types, and is mutually
exclusive with `~[async]` on the same type (see **Rules**, under **Async /
Await**) — a function is either suspendable-and-cooperative or
concurrent, never both.

```ebnf
type_qualifier  = '~[' type_qual_item { ',' type_qual_item } ']' type
type_qual_item  = 'async' | 'parallel' | 'nullable' | 'fallible'
```

### Body Restrictions

A function called through a `~[parallel]`-qualified binding has its body
restricted, because the runtime gives no guarantee about execution order or
which thread runs which invocation. Each restriction below exists to keep a
parallel body's effects fully local to its own invocation — without these,
concurrent invocations could race on shared state, return out of a context
the caller is no longer running, or block one invocation on another:

- **No `return` statements.** A parallel body has no single caller waiting on
  a single result the way a normal call does — there is no well-defined place
  for a `return` to deliver its value *to*. Produce output by mutating data
  passed in (through `&T`, see **Borrowed Types — Scoped References**), not
  by returning.
- **No `break` or `continue`.** These only have meaning relative to an
  enclosing loop in the *caller's* control flow. A parallel body runs
  independently of that loop's own iteration mechanics, so neither statement
  has anywhere left to target.
- **No `await` expressions.** Already implied by `~[async]`/`~[parallel]`
  being mutually exclusive on the same type, and by `await` only being valid
  inside an `~[async]`-qualified body (see **Rules**, under **Async /
  Await**) — a `~[parallel]` body can never also be `~[async]`, so `await`
  is unreachable inside one. Stated here explicitly since it is easy to
  forget when only thinking about the other three restrictions.
- **No writes to variables declared outside the body's own scope.** Lucid's
  closures can capture outer variables (see **Function Values and
  Closures**), which makes this restriction load-bearing rather than
  redundant: without it, a `~[parallel]` body capturing and writing to a
  shared outer variable would be a genuine, silent data race between
  concurrent invocations. Reading a captured outer variable is still
  permitted — only writing to it is forbidden.

```lucid
decl parallelFor<T> ~[const] (items [*]T)(body ~[parallel] (item T) -> ()) -> () = { ... }

decl result ~[mut] int = 0

parallelFor<Vertex>(mesh.vertices)((vertex Vertex) -> () {
    vertex.pos = scalePosition(vertex.pos)   -- OK: local to this invocation
    result = 5                                -- ERROR: write to outer-scope variable
    return                                    -- ERROR: return in a parallel body
    await fetch()                             -- ERROR: await in a parallel body
})
```

> [!NOTE]
> `vertex` itself is local to each invocation — it is the parameter the
> runtime hands to that invocation, not a captured outer variable, so writing
> to its fields is unrestricted. The restriction applies specifically to
> variables declared **outside** the body and reached through closure
> capture, such as `result` above.

---

## Grammar Summary (EBNF)

```ebnf
program         = { top_level_item }
top_level_item  = { attribute_list } { decl_qualifier } top_level_decl
                | use_decl
top_level_decl  = trait_decl | struct_decl | enum_decl | func_decl | var_decl
use_decl        = 'use' use_path [ 'as' IDENTIFIER ]
use_path        = IDENTIFIER { '.' IDENTIFIER }

attribute_list  = '@[' attr_item { ',' attr_item } ']'
decl_qualifier  = '~[' decl_qual_item { ',' decl_qual_item } ']'
attr_item       = IDENTIFIER [ '(' attr_args ')' ]
attr_args       = attr_arg { ',' attr_arg }
attr_arg        = STRING_LIT | INT_LIT | FLOAT_LIT | BOOL_LIT | IDENTIFIER
qual_item       = 'const' | 'mut' | 'async' | 'nullable' | 'parallel' | 'fallible'

trait_decl      = 'trait' IDENTIFIER [ generic_params ] '{' { trait_field } '}'
trait_field     = IDENTIFIER type
trait_ref       = IDENTIFIER | IDENTIFIER '<' type_arg { ',' type_arg } '>'

struct_decl     = 'struct' IDENTIFIER [ generic_params ] [ ':' trait_ref { ',' trait_ref } ] '{' { struct_field } '}'
struct_field    = { attribute_list } IDENTIFIER { decl_qualifier } type [ '=' expr ]

enum_decl       = 'enum' IDENTIFIER [ ':' int_type ] '{' { enum_variant } '}'
enum_variant    = { attribute_list } IDENTIFIER '=' INT_LIT

func_decl       = 'decl' IDENTIFIER [ generic_params ] { decl_qualifier }
                  param_group { param_group } [ '->' return_type ] '=' func_body
                | 'decl' IDENTIFIER [ generic_params ] { decl_qualifier }
                  param_group '->' func_type '=' func_body
var_decl        = 'decl' IDENTIFIER { decl_qualifier } type [ '=' expr ]

param_group     = '(' [ param_list ] ')'
param_list      = param { ',' param } [ ',' variadic_param ] | variadic_param
param           = IDENTIFIER [ { decl_qualifier } ] type
variadic_param  = IDENTIFIER '...' type
return_type     = type | '(' type { ',' type } ')'
func_body       = '{' { statement } '}' | expr
func_type       = param_group { param_group } '->' return_type
                | param_group '->' func_type
                | param_group '->' return_type

generic_params  = '<' generic_param { ',' generic_param } '>'
generic_param   = IDENTIFIER | IDENTIFIER ':' trait_ref { '+' trait_ref }
generic_args    = '<' type { ',' type } '>'

type            = primitive_type | IDENTIFIER | generic_type | func_type
                | array_type | ptr_type | tuple_type | qualified_type
                (* IDENTIFIER may refer to a trait — valid as field type, param type,
                   return type, and generic constraint *)
primitive_type  = int_type | float_type | 'bool' | 'byte' | 'string' | 'char'
int_type        = 'int8' | 'int16' | 'int32' | 'int64'
                | 'uint8' | 'uint16' | 'uint32' | 'uint64'
                | 'int' | 'uint' | 'byte' | 'short' | 'long' | 'ubyte' | 'ushort' | 'ulong'
float_type      = 'float' | 'double' | 'decimal'
array_type      = '[' array_size ']' type
array_size      = '*' | '_' | INT_LIT
ptr_type        = 'ptr' '<' type '>'
generic_type    = IDENTIFIER '<' type_arg { ',' type_arg } '>'
qualified_type  = '~[' qual_item { ',' qual_item } ']' type | type '?' | type '!' | type '?' '!'
tuple_type      = '(' type ',' type { ',' type } ')'

statement       = var_decl | func_decl | assign_stmt | return_stmt
                | if_stmt | switch_stmt | while_stmt | for_stmt
                | do_while_stmt | break_stmt | continue_stmt
                | expr_stmt | await_stmt
assign_stmt     = expr '=' expr | expr op_assign expr
op_assign       = '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^=' | '<<=' | '>>='
return_stmt     = 'return' [ expr { ',' expr } ]
break_stmt      = 'break'
continue_stmt   = 'continue'
expr_stmt       = expr
await_stmt      = 'await' expr

if_stmt         = 'if' expr block { 'else' 'if' expr block } [ 'else' block ]
while_stmt      = 'while' expr block
do_while_stmt   = 'do' block 'while' expr
for_stmt        = 'for' IDENTIFIER type 'in' range_iter [ '..' expr ] block
                | 'for' IDENTIFIER type 'in' expr block
                | 'for' IDENTIFIER type ',' IDENTIFIER type 'in' expr block
range_iter      = expr range_op expr
block           = '{' { statement } '}'
switch_stmt     = 'switch' expr '{' { case_clause } [ default_clause ] '}'
case_clause     = 'case' case_value { ',' case_value } ':' block
default_clause  = 'default' ':' block
case_value      = literal | IDENTIFIER '.' IDENTIFIER | literal range_op literal

expr            = literal | IDENTIFIER | call_expr | index_expr | field_expr
                | module_expr | unary_expr | binary_expr | func_literal | struct_literal
                | array_literal | tuple_expr | pipeline_expr | compose_expr
                | fallback_expr | generic_expr | range_expr | await_expr | '(' expr ')'
call_expr       = expr '(' [ arg_list ] ')'
arg_list        = expr { ',' expr }
index_expr      = expr '[' expr ']' | expr '[' [ expr ] range_op [ expr ] ']'
range_expr      = expr range_op expr
range_op        = '..' | '..<'
field_expr      = expr '.' IDENTIFIER
module_expr     = IDENTIFIER ':' IDENTIFIER | IDENTIFIER ':' call_expr | IDENTIFIER ':' generic_expr
unary_expr      = ( '-' | 'not' | '~' ) expr
binary_expr     = expr binary_op expr
binary_op       = '+' | '-' | '*' | '/' | '%' | '==' | '!=' | '<' | '<='
                | '>' | '>=' | 'and' | 'or' | '&' | '|' | '^' | '<<' | '>>'
func_literal    = param_group { param_group } '->' type block
                | param_group '->' func_type block
struct_literal  = IDENTIFIER [ generic_args ] '{' { field_init } '}'
field_init      = IDENTIFIER '=' expr
array_literal   = '[' [ expr { ',' expr } ] ']'
tuple_expr      = '(' expr ',' expr { ',' expr } ')'
pipeline_expr   = expr { '|>' expr }
compose_expr    = expr '+>' expr
fallback_expr   = expr '??' expr | expr '??' block
generic_expr    = IDENTIFIER generic_args '(' [ arg_list ] ')'
await_expr      = 'await' expr
```