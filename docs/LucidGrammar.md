# Lucid — Language Specification (Consolidated Draft)

This document is the whole language. Anything not described here is not part of Lucid. It supersedes every earlier draft in this design thread; where an earlier draft and this document disagree, this document wins.

---

## 1. Design summary

Lucid is a data-oriented scripting language for embedding in a game engine. It has exactly three declaration forms:

1. **Tables** — named, global, ordered collections of rows. A table has a fixed shape (columns) and either a fixed or growing set of rows. Tables are the only aggregate type; there is no separate struct or enum.
2. **Functions** — named procedures. A function takes one parameter group and produces zero or one result. No currying, no nesting, no closures.
3. **Variables** — named bindings (`let`/`const`) holding a primitive value or a table/row reference.

Everything else — operators, host calls, event callbacks, the IDE sheet view — is either a built-in operation on tables, or a compiler directive expressed as an attribute on one of the three declaration forms.

---

## 2. Lexical structure

### 2.1 Source form

Source files are UTF-8. A file is a sequence of declarations. There is no preprocessor, no include, no macro expansion.

### 2.2 Keywords

**Declaration keywords:**
```
TABLE   FN   let   const   import
```

**Type keywords:**
```
bool   char   string
int8   int16   int32   int64
uint8  uint16  uint32  uint64
float32   float64
unit
int   long   uint   ulong   float   double     -- sized aliases, see §5
```

These name the primitive types (§5). Like every other keyword, they are recognized directly by the lexer/parser and are never resolved as identifiers — nothing declares them, nothing registers them, and no code (script or host) can shadow or redefine them. This is also why a primitive carries no runtime indirection: it's stored inline at its natural size (§5.1), with none of the handle/registry machinery a `host(...)`-backed type (§4.1.2) uses.

**Statement keywords:**
```
if   else   switch   case   default
for   in   while
return   break   continue
```

**Sequence keywords** (see §9.2):
```
wait   waitFrames   waitUntil   waitForEvent   waitForRequest   start
```

**Literals:**
```
true   false   nil
```

**Modifier / target keywords:**
```
host        -- appears after '=' in a FN or TABLE target
```

The keyword set is closed. No keyword may be used as an identifier.

### 2.3 Identifiers

```
IDENTIFIER ::= LETTER { LETTER | DIGIT | '_' }
LETTER     ::= 'a'..'z' | 'A'..'Z' | '_'
DIGIT      ::= '0'..'9'
```

Identifiers are case-sensitive. `_` alone is a valid identifier, used as a discard binding in `for` loops.

### 2.4 Literals

```
INT_LIT    ::= DIGIT+ | '0x' HEX+ | '0b' BIN+ | '0o' OCT+
FLOAT_LIT  ::= DIGIT+ '.' DIGIT+ [ ('e'|'E') ['+'|'-'] DIGIT+ ]
STRING_LIT ::= '"' { STRING_CHAR } '"'
             | '"""' { ANY_CHAR } '"""'
CHAR_LIT   ::= '\'' ( CHAR_CHAR | ESCAPE ) '\''
BOOL_LIT   ::= 'true' | 'false'
NIL_LIT    ::= 'nil'

STRING_CHAR ::= ANY_CHAR_EXCEPT('"', '\', NEWLINE) | ESCAPE
CHAR_CHAR   ::= ANY_CHAR_EXCEPT('\'', '\')
ESCAPE      ::= '\' ( 'n' | 't' | 'r' | '\' | '\'' | '"' | '0' )
```

An untyped integer or float literal (§5.7) takes its concrete type from context. A `"..."` string processes escapes and forbids literal newlines. A `"""..."""` raw string processes neither and may span lines; its only forbidden content is `"""` itself.

### 2.5 Comments

```
line_comment  ::= '--' { ANY_CHAR } NEWLINE
block_comment ::= '/-' { ANY_CHAR | block_comment } '-/'    -- nestable
doc_comment   ::= '/--' { ANY_CHAR } '--/'
```

A doc comment attaches to the declaration that follows it.

### 2.6 Punctuation

```
( ) { } [ ]
, ; . @
: -> = ...  ??
+ - * / % **
== != < <= > >=
and or not
& | ^ ~ << >>
+= -= *= /= %= &= |= ^= <<= >>=
```

`::` is not in the set. Module member access uses `.` like every other member access.

### 2.7 Whitespace

Whitespace is not significant except as a token separator. Commas and semicolons are required where the grammar says so; elsewhere they may appear freely for readability.

---

## 3. Program structure

```
program        ::= { import_decl } { top_level_decl }
import_decl    ::= 'import' module_path [ 'as' IDENTIFIER ]
module_path    ::= IDENTIFIER { '.' IDENTIFIER }

top_level_decl ::= table_decl
                 | fn_decl
                 | var_decl
```

The top level contains only declarations. There are no top-level statements — nothing runs at module load time.

### 3.1 Modules

A file is a module. The file's path relative to the package root is the module's identity. There is no in-file `module` declaration.

```
import core.math
import entities.person as person
```

- `module_path` is the module's identity; the loader converts `core.math` to `core/math.luc`.
- The alias (after `as`) is the local name used to reach the module's members. Without an alias, the last path segment is the alias.
- Module member access uses `.`: `math.sqrt(...)`, `person.Person`.

### 3.2 Visibility

A declaration is visible outside its module only if marked `@export`. Everything else is private to its file. `@export` is the only visibility mechanism; there is no separate `public`/`private` keyword.

### 3.3 Resolution order and cyclic dependencies

Cyclic imports are allowed. Resolution happens in two passes across the whole import closure:

1. **Declaration pass.** For every module, collect table declarations, function signatures, and variable declarations. Resolve column types and parameter/return types against the module's import set. No function bodies are analyzed.
2. **Body pass.** For every module, analyze function bodies. Every name resolves against the full set of declarations from pass 1.

Because tables are references (§5.2), a cyclic type dependency (`TABLE A { b: &B }` and `TABLE B { a: &A }`) is trivially safe — each reference is a pointer, not an inline copy. Because signatures are resolved before bodies, a cyclic call dependency (`f` calls `g`, `g` calls `f`) is also safe.

### 3.4 Entry point and load tiers

**Lucid has no `main` and no concept of "the first file."** Nothing runs at module load time, so there is no ordering question to answer at the language level. Which module(s) are loaded, and what they're attached to, is entirely a host-side decision (a scene file, a manifest, an engine API call) — the same way scripting works in Unity or Godot.

Execution happens only through two host-driven mechanisms:

- **Direct call.** The host calls an `@export`ed function by name at a time of its choosing (an update tick, a game-specific hook).
- **Event callback.** The host calls an `@export`ed function tagged `@on(...)` (§9) when a matching event occurs.

**Loading is two-tiered when mods/extensions are involved:**

1. **Tier 1 — trusted workspace.** The host loads every core/game module first and fully resolves it (both passes, complete). This produces the full trusted declaration set and populates the complete host registry.
2. **Tier 2 — mods, resolved after Tier 1 is complete.** Each mod is resolved only against Tier 1's `@export`ed declarations plus its own. A mod cannot see another mod's private declarations or anything Tier 1 didn't export.

Dependency direction is strictly one-way: **mods depend on core; core never depends on a mod.** Only Tier 1 modules may register new `host(...)` natives. A Tier 2 mod only ever gets a *view* onto a subset of the registry that Tier 1 already populated, chosen by whoever loads the mod — this is also the trust boundary: a host loading untrusted mod content should scope the registry view down to whatever that mod actually needs (no raw file I/O, no arbitrary native calls, etc.), not hand it the full registry by default.

**Callback ordering.** When an event fires and multiple functions are tagged `@on(...)` for it, all Tier 1 callbacks run before any Tier 2 callbacks, in declaration order within each tier. This lets trusted code observe or veto behavior ahead of any mod.

---

## 4. Declarations

### 4.1 Table declarations

```
table_decl   ::= attribute_list 'TABLE' IDENTIFIER ( table_body | host_target )
table_body   ::= '{' { column } '}' [ table_init ]
table_init   ::= '=' '[' { row } ']'
column       ::= attribute_list IDENTIFIER ':' type
row          ::= '{' [ const_expr { ',' const_expr } ] '}'
host_target  ::= '=' 'host' '(' STRING_LIT ')'

const_expr   ::= literal
             | const_expr binary_op const_expr    -- primitive operands only
             | unary_op const_expr
             | IDENTIFIER '.' IDENTIFIER          -- T.Member on another FIXED table only
             | IDENTIFIER                         -- a top-level FN name, for function-typed columns
```

A table is a named, global container of rows. Its shape (column names and types, in source order) is fixed at declaration.

```
TABLE Person {
    name: string
    age:  int
}
```

#### 4.1.1 Growing vs. fixed tables

- **Growing** (no `table_init`): rows are added at runtime with `T.ADD(...)`.
- **Fixed** (`table_init` present, `= [ ... ]`): the row count is fixed by the declaration itself. `.ADD` and `.REMOVE` are not available. This is the enum replacement:

```
TABLE Direction {
    name: string
} = [
    { "North" }, { "East" }, { "South" }, { "West" }
]

let d: &Direction = Direction.North   -- resolved at compile time by the `name` cell
switch d {
    case Direction.North: { ... }
    default: { ... }
}
```

A table with inline rows is **implicitly `@readonly`** (frozen: no add, remove, or cell write) unless it is explicitly marked `@immutable` (cells may still be written; row count still cannot change).

#### 4.1.1a Fixed-table rows are constant expressions, and why

A `row`'s cells may only be built from `const_expr`: literals, arithmetic/unary operations on literals, `T.Member` references to *other fixed tables* (§7.1's compile-time fixed-row sugar), and — for a function-typed column (§5.0) — a bare top-level `FN` name. **Function calls and references to a growing table's contents are not allowed inside a fixed table's inline rows:**

```
TABLE Loadout {
    weapon: &Item
} = [
    { Item.byId(computeStartingWeapon()) }   -- error: function calls are not const_expr
]
```

A bare function name is allowed specifically because it isn't a call — `onIdleEnter` in §6.9's `StateHandler` example names a compile-time-known code address, the same as `Direction.North` names a compile-time-known row; neither one runs anything.

This isn't an arbitrary restriction — it's what makes fixed-table construction free at runtime. Because every inline row is resolvable entirely at compile time, the compiler evaluates it once during compilation and bakes the result into the compiled artifact as constant data — the same way a string literal is baked in, not constructed when the program starts. That has two consequences worth being explicit about:

- **There is no table load order to define.** Fixed tables aren't sequenced relative to each other or to growing tables at load time, because nothing about them runs at load time — their rows already exist as compiled constant data before the host loads anything (§3.4). Growing tables need no ordering either: they simply start as an empty header (row count zero), with no expression to evaluate.
- **A cross-reference between two fixed tables is a compile-time dependency**, resolved by the compiler the same way it already resolves `Direction.North` to a specific row. A genuine cycle between two fixed tables' constant rows (`A`'s row referencing `B.SomeMember` while `B`'s row references `A.SomeMember`) is a **compile error**, not a runtime problem — unlike the type-level cycles in §3.3, a *value* cycle between constants has no pointer trick to fall back on, so it's simply rejected.

#### 4.1.2 Host-backed tables

`TABLE X = host("name")` declares an opaque type whose storage lives on the C++ side. The script can hold, pass, and store values of this type, but cannot inspect or construct one — construction and inspection happen through host functions.

```
@export
TABLE SpriteRef = host("SpriteRef")
```

**This is only for a genuinely opaque handle — a value the script only ever passes around, never decomposes.** Primitive types (§2.2) are keywords, not declarations of any kind — they're a different case, not an example of this one. A native type in general falls into one of three shapes, each with its own convention; there is no fourth option, and in particular an opaque host type is never the right answer when the script needs to read or write fields:

| Native shape                                                    | Lucid convention                                                                                                                                                                                     |
| --------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Opaque handle — script never inspects it, only passes it around | `TABLE X = host("name")` (this section) — e.g. `SpriteRef`, `TextureRef`                                                                                                                             |
| Enum — a fixed, known set of values                             | An ordinary `FIXED TABLE` (§4.1.1) — the `Key`/`EventKind`/`Direction` pattern (§9.1, §11.2), with a host-side `@primary id` column if native code needs the value back as an integer                |
| Struct — fields the script needs to read or write               | An ordinary `TABLE X { ... }` with real columns, kept in sync through host functions (e.g. a native call that does `T.ADD(...)` from engine data, or writes into an existing row's cells each frame) |

If you find yourself wanting to read a field out of an opaque host type, that's a sign it should have been declared as a real `TABLE` with columns in the first place — not a reason to add field-access syntax to host types.

**One opaque handle can additionally be marked `@request`** (§4.1.4) if it represents a single, host-tracked async operation — a resource load, an RPC-style call. This is the host's promise that it will notify the runtime directly when that specific handle's operation completes, which is what makes `waitForRequest(req)` (§9.2.3) possible without polling:

```
@export @request
TABLE LoadRequest = host("LoadRequest")
```

#### 4.1.3 Table restrictions

- Column names are unique within a table.
- A column's type is fixed at declaration.
- Every row supplies a value for every column.
- Duplicate rows are allowed by default (this is a data container, not a set); use `@unique`/`@primary` on a column to forbid duplicates.
- A column's type may be a primitive, a host type, or a row reference (`&T`). **A column may not be a bare table type or an array type** — the former is a meaningless whole-sheet reference in a cell; the latter is modeled as a separate related table instead.

#### 4.1.4 Table attributes

| Attribute         | Meaning                                                                                                                               |
| ----------------- | ------------------------------------------------------------------------------------------------------------------------------------- |
| `@export`         | Visible outside the module.                                                                                                           |
| `@readonly`       | No mutation at all: no `ADD`, `REMOVE`, or cell write.                                                                                |
| `@immutable`      | No `ADD`/`REMOVE`; cells may still be written. Mutually exclusive with `@readonly`.                                                   |
| `@capped(N)`      | Upper bound of N rows for a growing table; `.ADD` fails once full. Mutually exclusive with inline rows (which already fix the count). |
| `@packed`         | Contiguous storage; valid only when every column is a primitive or host type.                                                         |
| `@sorted(column)` | Rows are kept sorted by `column`; `.ADD` inserts in order.                                                                            |
| `@request`        | Only valid on a `host(...)`-backed table (§4.1.2); marks it as a single-operation async handle usable with `waitForRequest` (§9.2.3). |

#### 4.1.5 Column attributes

| Attribute   | Meaning                                                                                                                                                             |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `@unique`   | No two rows share a value in this column. Checked on `ADD`; a duplicate panics.                                                                                     |
| `@primary`  | Implies `@unique`; also generates a `T.by<Column>(value) -> &T` lookup (e.g. `@primary id:` generates `Person.byId(...)`). At most one `@primary` column per table. |
| `@readonly` | The column's cells cannot be written after the row is added.                                                                                                        |

**No `@default(expr)` attribute.** `T.ADD(args...)` has exactly one calling rule: argument count must equal column count, in order (§7.1) — no exceptions. A default-value attribute would carve an exception into that rule (some trailing columns optional, others not), for something that's already fully achievable as domain logic rather than storage — an ordinary wrapper function, the same pattern already used for every other table convenience (`FIND` predicates, lookups, aggregations):

```
FN addPerson(name: string) -> &Person {
    return Person.ADD(nextId(), name, 0)   -- 0 is the "default" age
}
```

This is a closed decision, not a deferral — a future version of `@default` would either duplicate this wrapper-function pattern or reopen `ADD`'s one-rule calling convention, and neither is worth it for what a one-line function already does.

#### 4.1.6 Worked example: reading, writing, adding, and looking up rows

This example uses two related tables — an item catalog and an inventory that references it — to show every basic operation together.

```
TABLE Item {
    @primary
    id:    int

    @unique
    name:  string

    price: float
}

TABLE InventorySlot {
    item:  &Item
    count: int
}

-- ADD: build the catalog. Each ADD returns a &Item you can keep using.
let sword:  &Item = Item.ADD(1, "Iron Sword", 45.0)
let potion: &Item = Item.ADD(2, "Health Potion", 5.0)

-- ADD again, this time into a different table, holding a reference
-- to a row in the first one (the foreign-key pattern from §4.1).
InventorySlot.ADD(sword, 3)
InventorySlot.ADD(potion, 10)

-- READ a cell.
println(sword.name)                  -- "Iron Sword"

-- WRITE a cell — mutates the row in place. Anyone else holding a
-- reference to this row (e.g. every InventorySlot.item pointing at
-- it) sees the change immediately, because rows are shared, not copied.
sword.price = 40.0

-- LOOK UP a row later by its @primary column instead of holding
-- onto the reference from ADD. Always check for nil — by<Column>
-- returns nil rather than panicking when nothing matches (§7.3).
let found: &Item = Item.byId(2)
if found == nil {
    println("no such item")
} else {
    println(found.name)              -- "Health Potion"
}

-- ITERATE every row and mutate through a nested reference:
-- 10% off every item currently sitting in someone's inventory.
for slot: &InventorySlot in InventorySlot {
    slot.item.price = slot.item.price * 0.9
}

-- FIND a view of matching rows and iterate just that subset.
let bulky: InventorySlot = InventorySlot.FIND((s) -> s.count > 5)
for s: &InventorySlot in bulky {
    println(s.item.name)             -- "Health Potion"
}
```

Note what each operation returns and why: `ADD` gives you a row reference so you don't need a second lookup to keep working with the row you just created; `byId` gives you `nil` on a miss because "no item with this id" is an expected outcome, not a bug; and `slot.item.price = ...` chains a row-level write through a cell that itself holds a row reference — that's the whole foreign-key mechanism, with no special syntax beyond ordinary field access.

### 4.2 Function declarations

```
fn_decl    ::= attribute_list 'FN' IDENTIFIER '(' [ param_list ] ')'
               [ '->' type ] fn_body
fn_body    ::= block
             | '=' 'host' '(' STRING_LIT ')'

param_list ::= param { ',' param } [ ',' '...' type ]
param      ::= [ 'const' ] IDENTIFIER ':' type
```

A function is a named procedure with one parameter group, an optional return type, and a body.

```
FN createPerson(name: string, age: int) -> &Person {
    return Person.ADD(name, age)
}

FN incrementAge(const p: &Person) {
    p.age = p.age + 1     -- compile error: p is a const parameter
}
```

#### 4.2.1 Return

- No `-> type`: the function returns `unit`.
- `-> type`: the function returns one value. `-> unit` may be written explicitly for symmetry with the no-arrow form.
- A function's declared return type is a primitive, a row reference, or `unit`. A function never returns a bare table (copying a whole sheet isn't a meaningful operation).

#### 4.2.2 Parameters and `const`

A parameter with no qualifier may be mutated through (and, if it's a row reference, the referenced row may be mutated through it). A `const` parameter may not be reassigned or mutated through, regardless of what the caller passes.

**A `const`-bound argument cannot be passed to a non-`const` parameter.** This is checked at the call site, and is what makes `const` meaningful across function boundaries rather than only within one scope:

```
let  a: &Person = Person.ADD("alice", 30)
const b: &Person = a

giveRaise(a, 500)     -- OK
giveRaise(b, 500)     -- error: b is const, giveRaise's parameter is not
```

The last parameter may be variadic: `...type` collects zero or more trailing arguments into a `[type]` array.

```
FN sum(nums: ...int) -> int {
    let total: int = 0
    for n: int in nums {
        total += n
    }
    return total
}

sum(1, 2, 3)          -- nums = [1, 2, 3], returns 6
sum()                 -- nums = [], returns 0
```

A normal parameter may come before the variadic one, as long as the variadic parameter is last:

```
FN spawnEnemies(kind: string, positions: ...int) {
    for p: int in positions {
        spawnAt(kind, p)
    }
}

spawnEnemies("goblin", 10, 20, 30)
```

#### 4.2.3 Host functions

A function whose body is `= host("name")` is implemented by a native function registered under that name:

```
FN drawSprite(sprite: SpriteRef, x: int, y: int) = host("draw_sprite")
FN loadTexture(path: string) -> TextureRef = host("load_texture")
```

Sema checks that the name exists in the registry visible to this module (§3.4); the compiler does not otherwise interpret the string. There is exactly one mechanism for native interop — the same one whether the native function is an engine built-in or something a game/mod developer registered themselves.

#### 4.2.4 Variadic parameters on a host target

A variadic parameter and a `host(...)` body can be combined:

```
FN sum(nums: ...int) -> int = host("host_sum_ints")
```

**This works, but only through one fixed convention: a variadic parameter is always packed into an ordinary `[int]` array before the call crosses into native code, on every host-targeted function.** The registered C++ function is not itself a C-style variadic function (`...`) — it receives a fixed, two-value signature representing "the packed array," for example:

```cpp
int32_t host_sum_ints(const int32_t* data, size_t count);
```

The compiler always calls the native side this way for a variadic parameter, so `host_sum_ints` is written once, against a fixed pointer+count signature, regardless of how many arguments a particular script call site happens to pass. This is the same idea §4.2.3's plain host functions already rely on — the compiler shouldn't need to understand a target-platform's variadic calling convention, which differs by ABI and is exactly the kind of parser-unfriendly irregularity the fixed-operator-token decision (§6.10) was written to avoid elsewhere.

If a native function genuinely needs true C variadics — wrapping something like `printf` — that's outside this mechanism. Wrap it by hand on the C++ side into a fixed- or array-taking function first, then register that wrapper normally. The language does not provide a way to call a true variadic native function directly.

#### 4.2.5 No currying, no nesting, no closures

A function has one parameter group, cannot be declared inside another function, and has no captures — every name inside it resolves against its own parameters or its module's top-level declarations. A function value is a bare code pointer.

The one narrow exception is the lambda form (§6.9), which is sugar for a compiler-generated top-level function and follows the same no-capture rule.

#### 4.2.6 Function attributes

| Attribute               | Meaning                                                                              |
| ----------------------- | ------------------------------------------------------------------------------------ |
| `@export`               | Visible outside the module.                                                          |
| `@deprecated(msg)`      | Using the function produces a compile warning with `msg`.                            |
| `@on(EventKind.Member)` | Registers the function as a callback for the named event (§9.1). Requires `@export`. |
| `@sequence`             | Declares a suspension-capable function — see §9.2.                                   |

### 4.3 Variable declarations

```
var_decl ::= ( 'let' | 'const' ) IDENTIFIER ':' type '=' expr
```

A variable holds a value: a primitive (copied), or a row/table reference (shared).

- `let` — the binding may be reassigned; if it holds a reference, mutation through it is allowed.
- `const` — the binding may not be reassigned, and no mutation through it is allowed.

`const` restricts the *binding*, not the underlying data:

```
let   a: &Person = Person.ADD("alice", 30)
const b: &Person = a

a.age = 31       -- OK: a is let
b.age = 31       -- error: b is const

-- The row itself was changed; a and b refer to the same row.
```

`const` on a primitive and `const` on a reference are the same construct — both are read-only bindings; the grammar does not distinguish them.

---

## 5. Types

```
type           ::= primitive_type
                 | table_type
                 | row_ref_type
                 | array_type

primitive_type ::= 'bool' | 'char' | 'string'
                 | 'int8'   | 'int16'  | 'int32'  | 'int64'
                 | 'uint8'  | 'uint16' | 'uint32' | 'uint64'
                 | 'float32' | 'float64'
                 | 'unit'
                 -- sized aliases: int=int32, long=int64, uint=uint32,
                 --                ulong=uint64, float=float32, double=float64

table_type     ::= IDENTIFIER                -- a sheet
row_ref_type   ::= '&' IDENTIFIER             -- a reference to one row

array_type     ::= '[' ']' type               -- dynamic array
                 | '[' INT_LIT ']' type       -- fixed-size array

function_type  ::= '(' [ type { ',' type } ] ')' '->' type
```

### 5.0 Function types

A `function_type` is the type of a function value — a reference to a specific, compiler-known `FN` or lambda, identified by its signature. Because every function is already a bare code pointer with no captures (§4.2.5), a function value is a compile-time-known address tagged with its signature — nothing is allocated or captured at the point of assignment:

```
FN isMinor(p: &Person) -> bool { return p.age < 18 }
FN isAdult(p: &Person) -> bool { return p.age >= 18 }

let pred: (&Person) -> bool = isMinor
```

A function-typed value can be reassigned to any named `FN` or lambda whose parameter and return types match exactly — no implicit conversion between different function types, same as everywhere else in the type system (§5.7).

### 5.1 The reference model

Removing value references (`&int`) — see 5.1.1 below — leaves exactly two reference kinds, both spelled with identifiers rather than a shared ambiguous sigil doing double duty:

| Type                           | Storage                   | Copy semantics                                                                |
| ------------------------------ | ------------------------- | ----------------------------------------------------------------------------- |
| Primitive (`int`, `bool`, ...) | Inline                    | Copy the value                                                                |
| Table (`Person`)               | Pointer to sheet          | Copy the pointer (share the sheet)                                            |
| Row reference (`&Person`)      | Pointer to row            | Copy the pointer (share the row)                                              |
| Function ((`&T`) `-> R`)       | Compile-time code address | Copy the address — never allocated, never captures (§6.9)                     |
| Array (`[T]`, `[N]T`)          | Depends on `T`            | Element-wise copy for primitives; pointer copy per element for row references |
| Host type                      | Opaque                    | Host-defined                                                                  |

`Person` names the sheet; `&Person` names a reference to one of its rows. Both are references under the hood, but they refer to different things, and the type name makes that explicit at every use site.

#### 5.1.1 No value references

There is no `&int`, `&string`, etc. Primitives are always copied. If a function needs to mutate a caller's data, it takes a row reference and writes a cell — there is no other way to achieve "output parameter" semantics, and none is needed.

### 5.2 Nilability

**There is no `T?` or `T!` type suffix.** Instead, every row-reference type (`&T`) is inherently nilable — `nil` is an ordinary value of any `&T` type, the same way a null pointer is an ordinary value of a pointer type. This is a property `&T` already has, not a second type layered on top of it.

- `T.byId(...)`-style lookups (`@primary`, §4.1.5) return `&T`, and are `nil` when nothing matches.
- A row reference stored in a cell becomes `nil` if the row it pointed to is removed (§7.6).
- `nil` is compared with `==`/`!=` (§6.8) and defaulted with `??` (§5.3).

Bare table types and primitives are never nilable.

### 5.3 The `??` operator

`??` is the null-coalescing operator, nothing else:

```
expr ?? fallback
```

If `expr` (a row-reference-typed expression) is `nil`, evaluate and return `fallback`. Otherwise return `expr` unchanged. There is no `if cond ?? a else b` ternary form — that reused `??` for an unrelated second meaning and is removed; use a plain `if` statement.

### 5.4 Arrays

- `[T]` — dynamic; grows and shrinks via `.ADD`/`.REMOVE` (§8).
- `[N]T` — fixed-size, `N` a compile-time constant; supports indexing, `.COUNT()`, `.CONTAINS()`, and iteration, but not `.ADD`/`.REMOVE`.

Array literals are first-class expressions: `[1, 2, 3]`. An empty `[]` requires a type context to infer the element type.

### 5.5 Column views have no storable type

`T.column` (§7.4) produces a live view over a column's values, usable only inline in a `for` loop or an aggregation call (`.SUM()`, `.AVG()`). It cannot be assigned to a variable or passed as an argument — it is not a value of any type in this grammar, dynamic array included, because unlike an array it is a live view rather than a copy. To obtain a real, storable `[T]` copy of a column, call `.toArray()` on it.

### 5.6 Host types

A host type is declared with `TABLE X = host("name")` (§4.1.2). The script can hold and pass values of this type but not inspect or construct them.

### 5.7 Numeric literals and coercion

**No implicit coercion between two already-typed values.** `int + float` is a compile error; use an explicit conversion function (`toFloat(x) + y`, §11.1).

**Integer and float literals are untyped until context fixes them.** `42` is not pre-typed as `int32` and then rejected in a `uint` or `long` context — it adapts to whatever concrete numeric type the surrounding context requires:

```
let a: uint  = 42     -- OK, 42 adapts to uint
let b: long  = 42     -- OK, 42 adapts to long
let c: uint  = a + 1  -- OK, 1 adapts to uint to match a
let d: float = a      -- error: a is already uint32; no implicit coercion
```

---

## 6. Expressions

```
expr          ::= literal
                | identifier_expr
                | table_access
                | field_access
                | index_expr
                | call_expr
                | array_literal
                | row_literal          -- only inside table_init, see §4.1
                | lambda_expr
                | start_expr           -- see §9.2
                | unary_expr
                | binary_expr
                | paren_expr

literal       ::= INT_LIT | FLOAT_LIT | STRING_LIT | CHAR_LIT | BOOL_LIT | NIL_LIT

identifier_expr ::= IDENTIFIER

table_access  ::= IDENTIFIER                    -- the sheet itself
                | IDENTIFIER '.' IDENTIFIER      -- method call, column access, or fixed-row sugar
                | IDENTIFIER '[' expr ']'        -- a row by index

field_access  ::= expr '.' IDENTIFIER
index_expr    ::= expr '[' expr ']'

call_expr     ::= expr '(' [ arg_list ] ')'
arg_list      ::= expr { ',' expr }

array_literal ::= '[' [ expr { ',' expr } ] ']'

lambda_expr   ::= '(' [ param_list ] ')' '->' expr

unary_expr    ::= ( '-' | 'not' | '~' ) expr
binary_expr   ::= expr binary_op expr
binary_op     ::= '+' | '-' | '*' | '/' | '%' | '**'
                | '==' | '!=' | '<' | '<=' | '>' | '>='
                | 'and' | 'or'
                | '&' | '|' | '^' | '<<' | '>>'
                | '??'

paren_expr    ::= '(' expr ')'
```

### 6.1–6.7 (unchanged from earlier drafts)

Identifier, table-access, field-access, index, call, and array-literal expressions behave as previously specified: `Person` names the sheet; `Person.ADD(...)`/`Person[i]`/`Person.column` are the sheet operations (§7); `p.name` reads or writes a cell of the row `p`; `f(args)` calls a function.

### 6.8 Equality and comparison

| Operators         | Valid operand types                  | Meaning                                                                               |
| ----------------- | ------------------------------------ | ------------------------------------------------------------------------------------- |
| `==` `!=`         | primitives                           | value equality                                                                        |
| `==` `!=`         | `&T` (including against `nil`)       | identity — do both sides refer to the same row (or is one/both `nil`)                 |
| `==` `!=`         | `[T]` / `[N]T`                       | structural: same length, each element equal (element-wise identity for `&T` elements) |
| `==` `!=`         | function types                       | identity — same underlying `FN`/lambda or not                                         |
| `<` `<=` `>` `>=` | numeric primitives, `string`, `char` | ordering (lexicographic for `string`)                                                 |

Comparing two bare table-typed expressions (`Person == Person`) is a compile error — there is exactly one sheet per table name, so the comparison is always trivially true and carries no information. Ordering operators are not defined for `&T`, table, or array types.

### 6.9 Lambda expressions and function values

A lambda is a single-expression, no-capture function value, legal anywhere a `function_type` (§5.0) is expected — a `FIND` predicate, a function parameter typed `(...) -> R`, or a `let`/`const` binding:

```
Person.FIND((p) -> p.age < 18)

let pred: (&Person) -> bool = (p) -> p.age < 18
```

Inside a lambda body, only its own parameters and module-level declarations are visible — no enclosing local variables. This means a lambda desugars directly into an ordinary compiler-generated top-level function; it introduces no new runtime concept and keeps "every function is a bare code pointer" (§4.2.5) true.

**A named `FN` is just as valid wherever a function type is expected — this is the general mechanism, not a `FIND`-only special case:**

```
FN isMinor(p: &Person) -> bool { return p.age < 18 }
FN isAdult(p: &Person) -> bool { return p.age >= 18 }

FN countWhere(t: Person, pred: (&Person) -> bool) -> uint {
    return t.FIND(pred).COUNT()
}

countWhere(Person, isMinor)
countWhere(Person, isAdult)
countWhere(Person, (p) -> p.age == 18)   -- a lambda works at the same call site
```

Because a function value is a compile-time-known address rather than something constructed at runtime, a bare function name is also a valid `const_expr` (§4.1.1a) — a fixed table can hold behavior directly:

```
TABLE StateHandler {
    name:    string
    onEnter: (&Enemy) -> unit
} = [
    { "Idle",   onIdleEnter },
    { "Attack", onAttackEnter },
    { "Flee",   onFleeEnter },
]
```

This is a state machine expressed as a plain fixed table — no `switch` needed to dispatch on state, and the designer sees which function each state calls directly in the sheet view.

### 6.10 Operators are fixed tokens

Operators are not user-overloadable and are not resolvable identifiers — they are fixed lexical tokens the parser recognizes independent of what's in scope. `and`/`or`/`not` are keywords, not identifiers, specifically so the parser's notion of "what's an operator" never depends on name resolution.

### 6.11 Precedence

Highest to lowest:

| Level | Operators                    | Associativity |
| ----- | ---------------------------- | ------------- |
| 8     | postfix: `(` `)` `[` `]` `.` | left          |
| 7     | unary `-` `not` `~`          | right         |
| 6     | `**`                         | right         |
| 5     | `*` `/` `%`                  | left          |
| 4     | `+` `-`                      | left          |
| 3     | `==` `!=` `<` `<=` `>` `>=`  | left          |
| 2     | `and`                        | left          |
| 1     | `or`                         | left          |
| 0     | `??`                         | left          |

---

## 7. Table operations

### 7.1 Sheet-level operations

| Operation             | Result       | Notes                                                                                                                                                   |
| --------------------- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `T.ADD(args...)`      | `&T`         | Append a row. Not available on fixed or `@readonly` tables.                                                                                             |
| `T.REMOVE(i)`         | `unit`       | Remove row `i`; rows after it shift down by one. Not available on fixed or `@readonly` tables.                                                          |
| `T[i]`                | `&T`         | Row `i` by index. **Panics** if `i` is out of bounds — this never returns `nil`.                                                                        |
| `T.at(i)`             | `&T`         | Row `i` by index; returns `nil` instead of panicking if `i` is out of bounds.                                                                           |
| `T.COUNT()`           | `uint`       | Number of rows.                                                                                                                                         |
| `T.FIND(pred)`        | `T`          | A live view of rows matching `pred: (&T) -> bool` — a lambda or a named `FN` (§6.9). No copy; invalidated by a subsequent `REMOVE` on the parent table. |
| `T.by<Column>(value)` | `&T`         | Generated when a column has `@primary` (e.g. `byId`); `nil` if no row matches.                                                                          |
| `T.column`            | (view, §5.5) | Iterable/aggregable view over one column's values across all rows.                                                                                      |
| `T.Member`            | `&T`         | Fixed-table sugar: resolves to the row whose first `string` column equals `"Member"`, at compile time.                                                  |

### 7.2 Row-level operations

| Operation          | Result | Notes                                                                                 |
| ------------------ | ------ | ------------------------------------------------------------------------------------- |
| `r.column`         | value  | Read a cell.                                                                          |
| `r.column = value` | `unit` | Write a cell. Not available if the table is `@readonly` or the column is `@readonly`. |

### 7.3 Panic vs. nil — which operations do which

**Panics** (bugs, not recoverable in-language — see §10): `T[i]` out of bounds, a duplicate `@unique`/`@primary` value on `ADD`, exceeding `@capped(N)`, and dereferencing a `nil` row reference (`"attempt to access a nil value"`).

**Returns `nil`** (an expected, checkable absence): `T.at(i)` out of range, `T.by<Column>(value)` with no match, a cell whose referenced row was removed.

### 7.4 Column views and aggregation

```
for n: string in Person.name { println(n) }

let totalAge: int   = Person.age.SUM()
let avgAge:   float = Person.age.AVG()
let names:    [string] = Person.name.toArray()
```

### 7.5 `FIND` and views

```
let minors: Person = Person.FIND((p) -> p.age < 18)
for p: &Person in minors { println(p.name) }
```

The view is an index list into the parent table — cheap, no allocation of row data. **A `REMOVE` on the parent table invalidates any view taken before it**; re-`FIND` after removing rows if you need a fresh view.

### 7.6 Row removal and reference null-out

When `T.REMOVE(i)` runs, every cell across every table that held a `&T` reference to that specific row becomes `nil`. The compiler tracks which columns are row-reference-typed at compile time; the runtime maintains a small reference index (row → referencing cells) so this is proportional to the number of live references, not to the size of any table.

### 7.7 Iteration

```
for r: &Person in Person { ... }
for r: &Person in Person.FIND(pred) { ... }
for v: int in Person.age { ... }
for i: uint, r: &Person in Person { ... }
```

The `for` binding over a table or view is always a row reference; mutating through it mutates the underlying row. **The declared binding type must be exactly `&T`, never bare `T`** — iterating `Person` or a `FIND` view of it never produces a `Person` (a whole sheet), only `&Person` (one of its rows):

```
for p: Person in Person { ... }     -- error: iterating Person yields &Person, not Person
```

This is caught by Sema, not the parser, since `binding` accepts any `IDENTIFIER ':' type` — a bare `T` binding is exactly correct when the iterable is instead an *array* of row references (§8.2), where the array's own element type already is `&T`.

---

## 8. Array operations

| Operation         | Result | Available on                |
| ----------------- | ------ | --------------------------- |
| `arr[i]`          | `T`    | `[T]`, `[N]T`               |
| `arr.ADD(x)`      | `unit` | `[T]` only                  |
| `arr.REMOVE(i)`   | `unit` | `[T]` only                  |
| `arr.COUNT()`     | `uint` | `[T]`, `[N]T`               |
| `arr.CONTAINS(x)` | `bool` | `[T]`, `[N]T` (linear scan) |

These reuse the table's own vocabulary (`ADD`/`REMOVE`/`COUNT`) rather than a second naming convention, so every collection in the language looks the same from the outside.

### 8.1 Worked example: reading, writing, adding, and mutating in a loop

```
let scores: [int] = [10, 20, 30, 40]

-- READ
println(scores[1])            -- 20

-- WRITE a single element
scores[1] = 25

-- ADD grows the array
scores.ADD(50)                 -- scores is now [10, 25, 30, 40, 50]

-- CONTAINS
if scores.CONTAINS(30) {
    println("found it")
}

-- REMOVE shifts later elements down, same as T.REMOVE on a table
scores.REMOVE(0)               -- scores is now [25, 30, 40, 50]
```

**Looping to modify every element needs the index form, not the value form.** For primitive element types, `for x: T in arr` binds a *copy* of each element — writing to `x` changes only the local loop variable, not the array:

```
-- DOES NOT modify `scores` — x is a copy of each element
for x: int in scores {
    x = x * 2
}
```

To mutate the array in place, iterate with the index and assign back through it:

```
-- Doubles every score in place
for i: uint, x: int in scores {
    scores[i] = x * 2
}
```

This is the array equivalent of the table pattern in §4.1.6: a table's `for r: &Person in Person` binding is a row *reference*, so writing `r.age = ...` mutates the underlying row directly — no index needed. An array of primitives has no such reference to hand out (§5.1.1 — primitives are always copied), so the index is how you get back to the slot you want to change.

### 8.2 `for`-binding type must match the array's element type

Unlike a table (§7.7, where the binding is always `&T` regardless of what `T` is), an array's binding type is whatever the array's own declared element type is — bare `T` for an array of primitives, `&T` for an array of row references:

```
let people: [&Person] = [alice, bob, carol]   -- an array of row references

for p: &Person in people {
    p.age = p.age + 1          -- OK: p is a row reference, mutates through it
}

for p: Person in people { ... }   -- error: people's element type is &Person, not Person
```

So a bare `T` binding is correct for an array (matching its element type) but always wrong for a table or view (§7.7) — the two rules look similar but resolve against different things: the array's declared element type versus the table's fixed "iterating always yields `&T`" rule.

---

## 9. Attributes

```
attribute_list ::= { attribute }
attribute      ::= '@' attr_name
attr_name      ::= IDENTIFIER [ '(' attr_arg { ',' attr_arg } ')' ]
attr_arg       ::= STRING_LIT | INT_LIT | FLOAT_LIT | BOOL_LIT
                 | IDENTIFIER { '.' IDENTIFIER }
```

Attributes are juxtaposed, not comma-separated in a bracket — there is exactly one way to write a multi-attribute declaration:

```
@export @on(EventKind.KeyDown)
FN onJump(key: Key) {
    if key == Key.W { jump() }
}
```

(Splitting across lines is equivalent; it's the same tokens with different whitespace, not a second grammar form.)

Attributes never change what the parser reads for the declaration that follows — they're metadata Sema interprets, not syntax that reshapes the declaration. The full set is listed in §4.1.4, §4.1.5, and §4.2.5.

### 9.1 Event callbacks

```
FIXED TABLE EventKind { name: string } = [
    { "KeyDown" }, { "KeyUp" }, { "NetworkMessage" }, ...
]
```

`@on(EventKind.Member)` on an `@export`ed function registers it as a callback for that event kind. `EventKind.Member` is resolved at compile time (§7.1's fixed-table sugar), so a typo is a name-resolution error, not a silently-ignored string. Multiple functions may register for the same event kind; see §3.4 for ordering across trust tiers.

Sema checks that the function's parameter list matches whatever signature the named event kind requires (defined per event kind in the standard library, §11.2).

### 9.2 Sequences (the suspension primitive)

This solves a narrower problem than concurrency: **one function that needs to pause partway through and resume later** — a cutscene, a scripted dialogue, a timed sequence of engine calls — as opposed to several things making progress at once. An ordinary `FN` cannot do this: the only two things a function can do are run to completion or panic, and blocking inside a call would freeze the whole engine rather than "pausing" anything. A sequence is a distinct function flavor that adds a real third option — suspend — with the compiler responsible for making that safe.

There are two different *reasons* a sequence pauses, and the design gives each its own suspend points rather than one generic `wait`:

- **Authored pacing** — a duration the designer chose on purpose (a fade should take exactly 1 second). Use `wait`/`waitFrames`.
- **Waiting on completion of something with an unknown duration** — a texture load, a network round-trip. There's no correct number to guess here, so don't reach for `wait(1.0)` as a stand-in for "probably done by then." Use `waitForRequest` (§9.2.3) if the operation gives you a handle, or `waitForEvent` (§9.2.3) if it's a category of event rather than one instance you started. Falling back to `waitUntil` with a polled condition is always *correct* but costs a check every tick — prefer the push-based forms whenever the host can tell you directly.

#### 9.2.1 Declaring a sequence

```
@sequence
FN playIntro() {
    fadeOutOver(1.0)
    wait(1.0)
    showDialogueLine("Welcome, traveler.")
    waitUntil(isDown, Key.Space)
    fadeInOver(1.0)
}
```

A `@sequence` function is written exactly like an ordinary one, except its body may contain the suspend points below, and it is subject to the restrictions in §9.2.5.

#### 9.2.2 Suspend points

```
suspend_stmt   ::= ( 'wait' | 'waitFrames' ) '(' expr ')'
                 | 'waitUntil' '(' expr ',' expr ')'
                 | 'waitForEvent' '(' expr ')'
                 | 'waitForRequest' '(' expr ')'
```

`wait`, `waitFrames`, `waitUntil`, `waitForEvent`, and `waitForRequest` are keywords (§2.2), not ordinary functions — like `break`/`continue`, the parser recognizes them directly, and Sema requires them to appear only inside a `@sequence` function's body (nested inside `if`/`while`/`for`/`switch` blocks within it is fine — there's still no nested *declaration*, per §12.5).

| Suspend point                    | Argument(s)                                                | Resumes when                                          | Cost                                                |
| -------------------------------- | ---------------------------------------------------------- | ----------------------------------------------------- | --------------------------------------------------- |
| `wait(seconds)`                  | `float`                                                    | That much real time (accumulated `dt`) has passed.    | Polled once per tick.                               |
| `waitFrames(n)`                  | `uint`                                                     | `n` engine ticks have elapsed.                        | Polled once per tick.                               |
| `waitUntil(pred, arg)`           | `pred: (T) -> bool`, `arg: T`                              | `pred(arg)` returns `true`, re-checked once per tick. | Polled once per tick.                               |
| `waitForEvent(EventKind.Member)` | a fixed-table member (§9.1)                                | The next time that event kind fires.                  | Zero-poll — registered once, resumed on fire.       |
| `waitForRequest(req)`            | `req: &T`, `T` an `@request`-attributed host type (§4.1.2) | The host signals that specific request as complete.   | Zero-poll — registered once, resumed on completion. |

**`waitUntil` takes its predicate and argument separately rather than as a closed-over lambda.** A lambda can only see its own parameters and module-level declarations (§6.9) — it cannot capture a local like a request handle you just created. Passing the value in explicitly (`waitUntil(isDown, Key.Space)`, `waitUntil(isLoaded, req)`) keeps the no-capture rule intact everywhere, including here: `pred` is an ordinary no-capture function or lambda, and `arg` is just another local the compiler already has to keep alive across the pause (§9.2.6).

#### 9.2.3 Push-based waiting: events and requests

`waitForEvent` and `waitForRequest` exist specifically so "waiting on completion" doesn't have to mean polling. Both register the suspended sequence once and do zero work until the host actively resumes it — no per-tick check at all.

**`waitForEvent(EventKind.Member)`** is for a *category* of event you didn't initiate yourself — the next inbound network message, the next key press, anything already modeled as an `@on(...)` event kind (§9.1). It shares the same registration the callback mechanism uses; a sequence parked on `waitForEvent` is, from the runtime's point of view, a one-shot listener that resumes and unregisters itself the moment that event kind next fires.

**`waitForRequest(req)`** is for a *specific instance* of an async host operation you started yourself and hold a handle to — a resource load, a single RPC-style call. A host type is eligible for this only if it's declared `@request` (§4.1.2), which is the host's promise that it will notify the runtime directly when that particular handle's operation finishes, rather than requiring the operation to be polled:

```
-- core/resources.luc
@export @request TABLE LoadRequest = host("LoadRequest")
@export FN requestLoadTexture(path: string) -> &LoadRequest = host("request_load_texture")
@export FN result(req: &LoadRequest) -> TextureRef           = host("load_result")
```

```
@sequence
FN playIntro() {
    let req: &LoadRequest = requestLoadTexture("intro_bg.png")
    waitForRequest(req)                 -- resumes the instant the load finishes, however long that took
    let bg: TextureRef = result(req)
    drawSprite(bg, 0, 0)

    wait(1.0)                            -- a deliberate dramatic beat — unrelated to how long loading took
}
```

This is the execution shape you described — issue the request, let the engine keep running, get notified the moment it's actually done, then continue — with no guessed duration and no per-tick check anywhere in the load path. `wait(1.0)` still appears right after it, and that's the point of §9.2's opening distinction: one is pacing, the other is completion, and they should never be the same call.

#### 9.2.4 Starting a sequence

```
start_expr ::= 'start' call_expr
```

A `@sequence` function cannot be called with ordinary call syntax — `playIntro()` on its own is a compile error, since an ordinary call implies "run to completion and give me the result now," which is exactly what a sequence doesn't do. It must be launched with `start`, which returns a handle to the running instance:

```
let handle: &Coroutine = start playIntro()

start playIntro()          -- fire-and-forget is also fine; the expression can be discarded
```

`&Coroutine` is an opaque host-backed handle (§4.1.2's opaque-handle convention), managed through ordinary stdlib functions rather than new syntax:

```
-- core/coroutine.luc
@export TABLE Coroutine = host("Coroutine")
@export FN stop(c: &Coroutine)           = host("coroutine_stop")
@export FN isDone(c: &Coroutine) -> bool = host("coroutine_is_done")
```

```
let intro: &Coroutine = start playIntro()
if skipRequested {
    stop(intro)
}
```

#### 9.2.5 Restrictions (v1)

These keep the feature to "suspend one sequence," not "a general concurrency system":

- **A `@sequence` function always returns `unit`.** No `-> T`. If a caller needs a result, have the sequence write it into a table row, or call an ordinary `FN` as its last step.
- **A `@sequence` function cannot have a `host(...)` body.** Its body is compiled Lucid; suspension only makes sense for code the compiler itself is lowering.
- **A `@sequence` function cannot call another `@sequence` function directly.** To compose sequences, `start` the other one and `waitUntil(isDone, handle)` — this avoids nested-state-machine composition in the compiler for v1, at the cost of one extra line at each composition point.
- **A `@sequence` function is not a valid function-typed value** (§5.0) — it can't be passed to `FIND` or stored in a function-typed column, since calling it means "start it," not "run and return a value."
- No `try`/`finally` exists anywhere in the language (§10), so `stop`ping a sequence mid-suspend runs no cleanup code — write any necessary teardown as something explicit the caller does after `stop`, not as an implicit guarantee of the primitive.

#### 9.2.6 How it's compiled (informative)

A `@sequence` function is lowered by the compiler into a small generated state machine: a struct holding a state id plus whichever local variables are live across a suspend point (found by ordinary liveness analysis), and a step function that runs from one suspend point to the next. The runtime keeps two lists rather than one: **polled** coroutines (paused on `wait`/`waitFrames`/`waitUntil`), advanced once per engine tick, in `start` order, before any `@on(EventKind.Update)` callbacks run that tick; and **parked** coroutines (paused on `waitForEvent`/`waitForRequest`), which do no work at all and aren't touched by the tick loop until the host or the event registration explicitly resumes them. Either way, advancement is cooperative and single-threaded: only one coroutine is ever actually executing at a time, in a fixed, deterministic order — there is no preemption and no data race to guard against, which is what keeps this from becoming the general concurrency model §13.1 already argued against.

---

## 10. Errors

Lucid has exactly two failure channels and no `try`/`catch`:

- **Panics** — programmer bugs: out-of-bounds `T[i]`, a `@unique`/`@primary` violation, a `@capped(N)` table already full, dereferencing `nil`. Not recoverable inside the script.
- **`nil`** — a legitimately absent result: `T.at(i)`, `T.by<Column>(...)`, a cell whose row was removed. Checked with `== nil` / `!= nil` or defaulted with `??`.

A panic unwinds only as far as the host call boundary: the specific `@export`ed function the engine invoked (directly, or via `@on(...)`) returns an error to the engine instead of crashing the whole process. There is no in-script exception handling beyond that.

---

## 11. Predeclared core

### 11.1 Core functions

```
FN println(s: string)                = host("host_println")
FN print(s: string)                  = host("host_print")
FN readLine() -> string              = host("host_read_line")
FN panic(msg: string)                = host("host_panic")

FN toStr(v: int)    -> string        = host("int_to_str")
FN toStr(v: float)  -> string        = host("float_to_str")
FN toStr(v: bool)   -> string        = host("bool_to_str")

FN toFloat(v: int)   -> float        = host("int_to_float")
FN toInt(v: float)   -> int          = host("float_to_int")

FN abs(v: int)   -> int              = host("int_abs")
FN abs(v: float) -> float            = host("float_abs")
```

`toStr`'s overload-by-argument-type is resolved by a compiler-known mechanism — it is the one overload the language has; nothing else in Lucid is overloaded.

### 11.2 Standard library modules

Input, rendering, and networking are not language features — they are ordinary modules using the same `host(...)` and `@on(...)` mechanisms any game or mod developer has access to:

```
-- core/input.luc
@export FIXED TABLE Key { name: string } = [ { "W" }, { "A" }, { "S" }, { "D" }, { "Space" } ]
@export FN isDown(key: Key) -> bool = host("input_is_down")
```

```
-- core/net.luc
@export TABLE NetMsg { channel: string, payload: string }
@export FN sendRequest(url: string, body: string) = host("net_send")
```

```
-- core/render.luc
@export TABLE SpriteRef = host("SpriteRef")
@export FN drawSprite(sprite: SpriteRef, x: int, y: int) = host("draw_sprite")
@export FN loadTexture(path: string) -> TextureRef = host("load_texture")
```

A game module does `import core.input` and calls `input.isDown(Key.W)`, or writes its own `@export @on(EventKind.KeyDown) FN onJump(...)`. If the engine gains a new subsystem later, that's a new library module, not a grammar change — the compiler itself has zero built-in knowledge of input, rendering, audio, or networking.

---

## 12. Statements

```
statement     ::= var_decl ';'
                | assign_stmt ';'
                | return_stmt ';'
                | break_stmt ';'
                | continue_stmt ';'
                | suspend_stmt ';'
                | if_stmt
                | switch_stmt
                | while_stmt
                | for_stmt
                | expr_stmt ';'
                | block

block         ::= '{' { statement } '}'

assign_stmt   ::= lvalue assign_op expr
lvalue        ::= IDENTIFIER
                | expr '.' IDENTIFIER
                | expr '[' expr ']'
assign_op     ::= '=' | '+=' | '-=' | '*=' | '/=' | '%='
                | '&=' | '|=' | '^=' | '<<=' | '>>='

return_stmt   ::= 'return' [ expr ]
break_stmt    ::= 'break' [ IDENTIFIER ]
continue_stmt ::= 'continue' [ IDENTIFIER ]
suspend_stmt  ::= ( 'wait' | 'waitFrames' ) '(' expr ')'
                | 'waitUntil' '(' expr ',' expr ')'
                | 'waitForEvent' '(' expr ')'
                | 'waitForRequest' '(' expr ')'          -- see §9.2

if_stmt       ::= 'if' expr block [ 'else' ( block | if_stmt ) ]
switch_stmt   ::= 'switch' expr '{' { case_clause } default_clause '}'
case_clause   ::= 'case' expr ':' block
default_clause ::= 'default' ':' block

while_stmt    ::= [ label ] 'while' expr block
for_stmt      ::= [ label ] 'for' binding { ',' binding } 'in' expr block
label         ::= IDENTIFIER ':'
binding       ::= IDENTIFIER ':' type | '_'

expr_stmt     ::= expr
```

### 12.1 Compound assignment

`lvalue op= expr` desugars to `lvalue = lvalue op expr`. Valid for every numeric and bitwise binary operator; not defined for `&T`, table, or array lvalues.

### 12.2 `switch`

`default` is always required — regardless of the check below, there is always a defined behavior for a case that isn't listed.

Each `case` expression is a constant expression (typically fixed-table sugar, `Direction.North`). **When the `switch` subject's type is `&T` for a specific fixed table `T`, Sema checks the `case` expressions against `T`'s full member list and emits a warning (not an error) if any member is missing:**

```
switch d {
    case Direction.North: { ... }
    case Direction.East:  { ... }
    default: { ... }
}
-- warning: switch over Direction does not cover all members (missing: South, West)
```

The check only applies when the subject's type is a specific fixed table — it doesn't run for a growing table (which has no fixed member list to check against) or for a `switch` over an ordinary primitive value. It's a warning rather than an error precisely because `default` is mandatory and always well-defined: the point isn't to forbid relying on `default`, it's to flag that a fixed table gained members since this `switch` was written, so you can confirm the fallthrough to `default` was intended rather than assumed.

### 12.3 `for` bindings

One or two bindings depending on the iterable — see §7.7 (tables/views) and §8 (arrays); a discard binding is `_`.

### 12.4 Loop labels

A `while` or `for` may be prefixed with a label — an identifier followed by `:` — so `break`/`continue` can target an outer loop from inside a nested one:

```
search: for i: uint, row: [int] in matrix {
    for j: uint, cell: int in row {
        if cell == target {
            println("found it")
            break search        -- exits the outer `for`, not just the inner one
        }
    }
}
```

`break` and `continue` without a label affect only the nearest enclosing loop, exactly as before — a label is purely opt-in. `continue label` jumps to the next iteration of the labeled loop rather than the innermost one:

```
rows: for r: &Person in Person {
    for s: &InventorySlot in InventorySlot {
        if s.item == nil { continue rows }   -- give up on this Person, move to the next one
        -- ... work with r and s together ...
    }
}
```

A label is its own small namespace: it never collides with a variable, function, or table name, and `break`/`continue`'s optional identifier is resolved only against enclosing labels, not against ordinary identifiers in scope.

### 12.5 Local declarations

`var_decl` may appear inside a block; its scope is the block. `FN` and `TABLE` may only appear at module (top) level — no nested declarations of either kind.

---

## 13. What this grammar deliberately excludes

- Traits, `DEF`, `REQUIRE`, `OpKind`, user-defined operator overloading.
- A `fn`/`cls` distinction, general closures, currying.
- `async`, `await`, `spawn`, `Deferred<T>`, threads, locks — see §13.1. (`start`/`wait`/`waitFrames`/`waitUntil` are adopted, but only as the narrow sequence primitive in §9.2, not a general concurrency system.)
- Generics.
- `T?`/`T!` type suffixes (nilability is a property of `&T`, not a suffix, §5.2).
- Value references (`&int`) — primitives are always copied.
- `@default(expr)` on a column (§4.1.5) — a default value is domain logic, expressed as an ordinary wrapper function, not storage metadata.

### 13.1 Why sequences (§9.2), not general concurrency

Every async-shaped need identified so far, apart from one, reduces to a synchronous call from the script's point of view once the callback mechanism (§9.1) exists:

- **Input** — an `@on(EventKind.KeyDown)` callback, called once per event.
- **Network** — sending is a fire-and-forget `host(...)` call; receiving is an `@on(EventKind.NetworkMessage)` callback when a response lands.

The one case that doesn't fit a callback is **sequencing** work across time inside one logical unit — "wait 2 seconds, then do X," for cutscenes and scripted dialogue — because that requires *pausing partway through a function's own body and resuming it later*, which a callback (which always returns control to the engine immediately) can't express. §9.2 answers that directly with `@sequence`/`wait`/`waitFrames`/`waitUntil`/`start`, rather than deferring it.

This is still a narrow, single-purpose addition, not general concurrency: `@sequence` functions cannot call each other directly (§9.2.4), the runtime advances every active sequence cooperatively and in a fixed, deterministic order once per tick (§9.2.5), and there is no preemption, no shared-memory data race, and nothing resembling a thread or a lock anywhere in the model. If a need ever arises that this doesn't cover — genuine parallel execution, for instance — that would be a different, much larger feature, and isn't part of this decision.

---

## 14. Remaining open items (deferred, not blocking)

These are extensions that can be added later without reshaping anything above:

1. Direct sequence-to-sequence composition (today, a `@sequence` composes another only via `start` + `waitUntil(isDone(...))`, §9.2.4 — a nested-composition form, if ever needed, is a bigger compiler change and deliberately not attempted yet).
2. Registry-scoping tooling for Tier 2 mods (the *policy* — one-way dependency, Tier-1-only registration — is settled in §3.4; the concrete host-side API for defining a mod's registry view is not part of this document).

---

*This document consolidates the full design conversation: the tables/functions/variables redesign, the sheet-vs-shape resolution, the attribute system, the `??`/nilability rework, lambdas and first-class function types, `switch` (with fixed-table exhaustiveness warnings), compound assignment, array operations, numeric coercion rules, the `@on(...)` event/callback mechanism, the `@sequence` suspension primitive, the host/standard-library split (including the opaque-handle/enum/struct convention for native types), and the Tier 1/Tier 2 loading model.*