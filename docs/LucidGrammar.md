# Lucid — Language Specification

The whole language. Anything not described here is not part of Lucid. Supersedes every earlier draft; where an earlier draft and this document disagree, this document wins.

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
FIXED   TABLE   FN   let   const   import
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

An untyped integer or float literal (§5.8) takes its concrete type from context. A `"..."` string processes escapes and forbids literal newlines. A `"""..."""` raw string processes neither and may span lines; its only forbidden content is `"""` itself.

**Lexing `.` vs. `..`/`..<` (range operators, §6.12).** A `FLOAT_LIT` requires at least one digit immediately after its decimal point; a bare `.` followed by another `.` never satisfies that, so the lexer resolves the ambiguity with one character of lookahead: after consuming a digit sequence and a `.`, if the next character is also `.`, back up and emit the digit sequence as an `INT_LIT` followed by a `..`/`..<` token, rather than attempting a `FLOAT_LIT`. This means `1..10` lexes as `INT_LIT(1)`, `..`, `INT_LIT(10)`, while `1.0` lexes as a single `FLOAT_LIT`.

**Lexing `?` vs. `??`.** A single `?` is the nilability suffix (§5.3). Two consecutive `?` are the null-coalescing operator (§5.4). The lexer resolves the ambiguity with one character of lookahead: after consuming a `?`, if the next character is also `?`, emit `??`; otherwise emit `?`.

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
: -> = ...  ?  ??
+ - * / % **
== != < <= > >=
and or not
& | ^ ~ << >>
+= -= *= /= %= &= |= ^= <<= >>=
.. ..<
```

`::` is not in the set. Module member access uses `.` like every other member access.

### 2.7 Whitespace

Whitespace is not significant except as a token separator. Commas and semicolons are required where the grammar says so; elsewhere they may appear freely for readability.

---

## 3. Program structure

```
program        ::= { import_decl | top_level_decl }
import_decl    ::= 'import' module_path [ 'as' IDENTIFIER ]
module_path    ::= IDENTIFIER { '.' IDENTIFIER }

top_level_decl ::= table_decl
                 | fn_decl
                 | var_decl
```

The top level contains only declarations (imports included). There are no top-level statements — nothing runs at module load time.

### 3.1 Modules

A file is a module. The file's path relative to the package root is the module's identity. There is no in-file `module` declaration.

```
import core.math
import entities.person as person
```

- `module_path` is the module's identity; the loader converts `core.math` to `core/math.luc`.
- The alias (after `as`) is the local name used to reach the module's members. Without an alias, the last path segment is the alias.
- Module member access uses `.`: `math.sqrt(...)`, `person.Person`.
- An `import_decl` may appear anywhere among a module's top-level declarations, not only at the top of the file — there is no separate "imports section" to keep in order. This falls directly out of §3.3's two-pass resolution: every import in a module is collected before any declaration's types are resolved, so where an `import` sits relative to other declarations in the source never affects what compiles.

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
table_decl   ::= attribute_list [ 'FIXED' ] 'TABLE' IDENTIFIER ( table_body | host_target )
table_body   ::= '{' { column } '}' [ table_init ]
table_init   ::= '=' '[' { row } ']'
column       ::= attribute_list IDENTIFIER ':' type
row          ::= '{' [ const_expr { ',' const_expr } ] '}'
host_target  ::= '=' 'host' '(' STRING_LIT ')'

const_expr   ::= literal
             | const_expr binary_op const_expr    -- primitive operands only
             | unary_op const_expr
             | qualified_table '.' IDENTIFIER     -- T.Member, or module.T.Member, on another FIXED table only
             | IDENTIFIER                         -- a top-level FN name, for function-typed columns
```

`qualified_table` (§5) is deliberately reused here rather than repeating a narrower two-level form: a fixed-table member reference inside another module's rows needs the same one-level module qualification a `&T` type does, e.g. `directions.Direction.North` inside a `table_init` in a module that `import`s `directions`.

A table is a named, global container of rows. Its shape (column names and types, in source order) is fixed at declaration.

```
TABLE Person {
    name: string
    age:  int
}
```

#### 4.1.1 Growing vs. fixed tables

A table is either **growing** or **fixed**. The `FIXED` keyword determines which:

- **Growing** (`TABLE X { ... }`): rows are added at runtime with `T.ADD(...)` and removed with `T.REMOVE(i)`. A growing table's storage is managed by the runtime; slots are reused after a `REMOVE` (§4.1.1a).
- **Fixed** (`FIXED TABLE X { ... }`): the row set is decided at declaration. `.ADD` and `.REMOVE` are not available. This is the enum replacement:

```
FIXED TABLE Direction {
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

**The presence of a `= [ ... ]` initializer does not determine fixedness.** A growing table may have an initializer, in which case the rows are its initial contents. A `FIXED` table has an initializer (its rows are its only rows), but the grammar does not require it — a `FIXED` table with no initializer is a zero-row table. Sema warns on a `FIXED` table with no rows (§4.1.1b).

**A `FIXED` table's row set is decided at declaration.** Its cells are writeable unless the table is also marked `@readonly`. The `FIXED` keyword does not by itself forbid cell writes.

**A growing table with an initializer is not implicitly readonly.** Its initial rows are the same as rows added later; both can be written and removed.

#### 4.1.1a Slot reclamation in growing tables

A growing table's storage is a **slot array**: a fixed-capacity buffer of rows, plus a free list of available slots, plus a generation counter per slot.

- **`ADD`** pops a slot from the free list, or appends a new slot if the free list is empty. It writes the row's data and marks the slot live.
- **`REMOVE(i)`** marks slot `i` dead and pushes it onto the free list. Every `&T` reference to the removed row becomes `nil` (the null-out rule, §7.6).
- **Slot reuse.** When a slot is reused for a new row, its generation counter is incremented. A stale `&T` reference (one whose generation doesn't match the slot's current generation) reads as `nil`. This makes a reference to a removed-and-reused row safely `nil` instead of silently aliasing a different row.

**Iteration order is slot order.** Rows are visited in the order of their slot indices, not in the order they were added. This order may change after a `REMOVE` and subsequent `ADD`. A program that needs a specific order must sort the table explicitly.

**A `FIXED` table has no slot array, no free list, and no generation counters.** Its rows are indexed 0..N-1 in declaration order, and its `&T` references are simple indices. A `FIXED` table's references are therefore smaller than a growing table's.

#### 4.1.1b Fixed tables: initializer and warnings

A `FIXED` table's row set is decided at declaration. The `= [ ... ]` initializer, if present, supplies those rows. A `FIXED` table without an initializer has zero rows.

**Sema warns on a `FIXED` table with no rows.** A table whose row set can never change and is empty is almost always a mistake. The warning is not a compile error: a zero-row fixed table is technically valid, and the program might use it as a placeholder.

**A `FIXED` table's rows are baked into the compiled artifact** (§4.1.1c). Their cells must be constant expressions.

#### 4.1.1c Constant expressions in a `FIXED` table's rows

A `FIXED` table's inline `row` cells may only be built from `const_expr`: literals, arithmetic/unary operations on literals, `T.Member` references to *other fixed tables* (§7.1's compile-time fixed-row sugar), and — for a function-typed column (§5.0) — a bare top-level `FN` name. **Function calls and references to a growing table's contents are not allowed inside a `FIXED` table's inline rows:**

```
FIXED TABLE Loadout {
    weapon: &Item
} = [
    { Item.byId(computeStartingWeapon()) }   -- error: function calls are not const_expr
]
```

A bare function name is allowed specifically because it isn't a call — `onIdleEnter` in §6.9's `StateHandler` example names a compile-time-known code address, the same as `Direction.North` names a compile-time-known row; neither one runs anything.

This isn't an arbitrary restriction — it's what makes fixed-table construction free at runtime. Because every inline row is resolvable entirely at compile time, the compiler evaluates it once during compilation and bakes the result into the compiled artifact as constant data — the same way a string literal is baked in, not constructed when the program starts. That has two consequences worth being explicit about:

- **There is no table load order to define.** Fixed tables aren't sequenced relative to each other or to growing tables at load time, because nothing about them runs at load time — their rows already exist as compiled constant data before the host loads anything (§3.4). Growing tables need no ordering either: they simply start as an empty header (row count zero), with no expression to evaluate.
- **A cross-reference between two fixed tables is a compile-time dependency**, resolved by the compiler the same way it already resolves `Direction.North` to a specific row. A genuine cycle between two fixed tables' constant rows (`A`'s row referencing `B.SomeMember` while `B`'s row references `A.SomeMember`) is a **compile error**, not a runtime problem — unlike the type-level cycles in §3.3, a *value* cycle between constants has no pointer trick to fall back on, so it's simply rejected.

**A growing table's `= [ ... ]` initializer is also evaluated at compile time.** Its rows are the same as `const_expr` rows in a fixed table; the difference is only that a growing table may add more rows at runtime. A growing table's initializer is a compile-time constant block, seeded into the runtime's storage at startup.

#### 4.1.2 Host-backed tables

`TABLE X = host("name")` declares an opaque type whose storage lives on the C++ side. The script can hold, pass, and store values of this type, but cannot inspect or construct one — construction and inspection happen through host functions.

```
@export
TABLE SpriteRef = host("SpriteRef")
```

A host-backed table may also be `FIXED`:

```
FIXED TABLE OpaqueHandle = host("OpaqueHandle")
```

A `FIXED` host-backed table is a fixed set of opaque handles. This combination is legal; it means the handles' identities are decided at declaration, not added at runtime. In practice, `FIXED TABLE X = host(...)` is unusual (an opaque handle is a value, not a row set), but it is permitted.

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
- A column's type may be a primitive, a host type, a row reference (`&T`), or an array (`[T]`, `[N, T]`). **A column may not be a bare table type or a function type** — the former is a meaningless whole-sheet reference in a cell; the latter is a code pointer that a data container should not hold (use a bare `FN` name in a function-typed column of a fixed table, §4.1.1c, or store a `&T` to a table row that holds the function value).
- **Any column type may be nilable (`T?`)** for primitives, host types, and arrays. A nilable cell may hold `nil`; a non-nilable one may not.
- **A `FIXED` table's row set is decided at declaration**; a growing table's is not.

**Array-typed columns are permitted.** A column of type `[T]` holds one array per row. The runtime stores the array's data in a shared buffer with per-row offsets (§7.4). This supports 2D and higher-dimensional data directly.

#### 4.1.4 Table attributes

| Attribute         | Meaning                                                                                                                                                                                                                                                              |
| ----------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `@export`         | Visible outside the module.                                                                                                                                                                                                                                          |
| `@readonly`       | No mutation at all: no `ADD`, `REMOVE`, or cell write. Implies `@immutable`.                                                                                                                                                                                         |
| `@immutable`      | No `ADD`/`REMOVE` after initialization; cells may still be written. Redundant on a `FIXED` table (whose row set is already fixed at declaration); Sema warns when `FIXED` and `@immutable` appear together. Mutually exclusive with `@readonly` (which is stricter). |
| `@packed`         | Contiguous storage with no slack. Implies `@immutable`.                                                                                                                                                                                                              |
| `@reserve(N)`     | A storage hint: reserve room for N rows. Not a policy limit.                                                                                                                                                                                                         |
| `@columnar`       | Store the table's columns in separate contiguous buffers, rather than row-major.                                                                                                                                                                                     |
| `@sorted(column)` | Rows are kept sorted by `column`; `.ADD` inserts in order.                                                                                                                                                                                                           |
| `@request`        | Only valid on a `host(...)`-backed table (§4.1.2); marks it as a single-operation async handle usable with `waitForRequest` (§9.2.3).                                                                                                                                |

**`@reserve(N)` is a storage hint, not a policy limit.** A table declared `@reserve(10)` has room reserved for 10 rows at startup; it can still grow past 10 (with reallocation), and it can have fewer than 10 rows. A policy limit ("the game allows at most 10 inventory slots") is expressed as ordinary code — a wrapper function that checks `InventorySlot.COUNT()` before calling `.ADD`. This mirrors the design's treatment of `@default` (there is no `@default` attribute; a default value is domain logic, expressed as a wrapper function).

**`@packed` implies `@immutable`.** A packed table's storage is finalized at initialization and never grows, shrinks, or reallocates. `ADD` and `REMOVE` are unavailable. Cell writes are allowed unless the table is also `@readonly`.

**`@packed` and `@readonly` are not mutually exclusive.** A `@packed @readonly` table is a fixed-layout, read-only lookup table, which is a common case (a constants table).

#### 4.1.5 Column attributes

| Attribute   | Meaning                                                                                                                                                                                                                                  |
| ----------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `@unique`   | No two rows share a value in this column. Checked on `ADD`; a duplicate panics.                                                                                                                                                          |
| `@primary`  | Implies `@unique`; also generates a `T.by<Column>(value) -> &T` lookup (e.g. `@primary id:` generates `Person.byId(...)`). At most one `@primary` column per table. The runtime builds an index over the column, making the lookup O(1). |
| `@readonly` | The column's cells cannot be written after the row is added.                                                                                                                                                                             |

**No `@default(expr)` attribute.** `T.ADD(args...)` has exactly one calling rule: argument count must equal column count, in order (§7.1) — no exceptions. A default-value attribute would carve an exception into that rule (some trailing columns optional, others not), for something that's already fully achievable as domain logic rather than storage — an ordinary wrapper function, the same pattern already used for every other table convenience (`FIND` predicates, lookups, aggregations):

```
FN addPerson(name: string) -> &Person {
    return Person.ADD(nextId(), name, 0)   -- 0 is the "default" age
}
```

This is a closed decision, not a deferral.

**No `@optional` attribute.** A column that may hold `nil` is written with the `?` type suffix (§5.3):

```
TABLE Person {
    name: string
    nick: string?      -- an optional nickname
    age:  int?         -- an optional age
}
```

Reading an `@optional` column's cell produces a `T?` value, which is narrowed by the usual rules (§6.13).

#### 4.1.6 Worked example: reading, writing, adding, and looking up rows

This example uses two related tables — an item catalog and an inventory that references it — to show every basic operation together.

```
FIXED TABLE Item {
    @primary
    id:    int

    @unique
    name:  string

    price: float
} = [
    { 1, "Iron Sword",    45.0 },
    { 2, "Health Potion",  5.0 },
]

TABLE InventorySlot {
    item:  &Item
    count: int
}

-- ADD: build the inventory. Each ADD returns a &InventorySlot you can
-- keep using.
let slot0: &InventorySlot = InventorySlot.ADD(Item.byId(1), 3)
let slot1: &InventorySlot = InventorySlot.ADD(Item.byId(2), 10)

-- READ a cell.
println(slot0.item.name)             -- "Iron Sword"

-- WRITE a cell — mutates the row in place. Anyone else holding a
-- reference to this row sees the change immediately, because rows are
-- shared, not copied.
slot0.count = 5

-- LOOK UP a row later by its @primary column instead of holding onto
-- the reference from ADD. Always check for nil — by<Column> returns
-- nil rather than panicking when nothing matches (§7.3).
let found: &Item = Item.byId(2)
if found != nil {
    println(found.name)              -- "Health Potion"
}

-- ITERATE every row and mutate through a nested reference:
-- 10% off every item currently sitting in the inventory.
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

#### 4.1.7 Quick reference on table's constraints

| Example                          | Note                                      |
| -------------------------------- | ----------------------------------------- |
| TABLE X { ... }                  | mutable (growable, cells writable).       |
| TABLE X { ... } @immutable       | no ADD/REMOVE after init; cells writable. |
| TABLE X { ... } @readonly        | no ADD/REMOVE, no cell writes.            |
| FIXED TABLE X { ... }            | fixed row set; cells writable.            |
| FIXED TABLE X { ... } @readonly  | fixed row set; cells read-only.           |
| FIXED TABLE X { ... } @immutable | redundant (a FIXED table is already       | immutable); Sema warns. |

### 4.2 Function declarations

```
fn_decl    ::= attribute_list 'FN' IDENTIFIER '(' [ param_list ] ')'
               [ '->' type ] fn_body
fn_body    ::= block
             | '=' 'host' '(' STRING_LIT ')'

param_list ::= param { ',' param } [ ',' variadic_param ]
param      ::= [ 'const' ] IDENTIFIER ':' type
variadic_param ::= IDENTIFIER ':' '...' type
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
- A function's declared return type is a primitive (possibly nilable), a row reference, an array, a host type, or `unit`. A function never returns a bare table. A function never returns a function type (no first-class functions, §4.2.5).

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

**A variadic parameter's element type may be nilable:** `...int?` collects into `[int?]`.

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

The compiler always calls the native side this way for a variadic parameter, so `host_sum_ints` is written once, against a fixed pointer+count signature, regardless of how many arguments a particular script call site happens to pass.

If a native function genuinely needs true C variadics — wrapping something like `printf` — that's outside this mechanism. Wrap it by hand on the C++ side into a fixed- or array-taking function first, then register that wrapper normally.

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
var_decl ::= ( 'let' | 'const' ) IDENTIFIER ':' type '=' expr ';'
```

A variable holds a value: a primitive (copied), or a row/table reference (shared), or an array (copied on assignment; §5.5).

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

**A `;` terminates every `let`/`const`, whether at top level or inside a block.** §3's `top_level_decl` and §12's `statement` both route through `var_decl`, which carries its own `;`.

---

## 5. Types

```
type           ::= base_type [ '?' ]

base_type      ::= primitive_type
                 | table_type
                 | row_ref_type
                 | array_type
                 | function_type

primitive_type ::= 'bool' | 'char' | 'string'
                 | 'int8'   | 'int16'  | 'int32'  | 'int64'
                 | 'uint8'  | 'uint16' | 'uint32' | 'uint64'
                 | 'float32' | 'float64'
                 | 'unit'
                 -- sized aliases: int=int32, long=int64, uint=uint32,
                 --                ulong=uint64, float=float32, double=float64

table_type     ::= qualified_table            -- a sheet
row_ref_type   ::= '&' qualified_table         -- a reference to one row
qualified_table ::= [ IDENTIFIER '.' ] IDENTIFIER   -- optional module alias, then table name

array_type     ::= '[' type ']'                -- dynamic array
                 | '[' INT_LIT ',' type ']'    -- fixed-size array

function_type  ::= '(' [ type { ',' type } ] ')' '->' type
```

**Table names are unique only within their own module**, the same as every other declaration (§3.3) — two modules can each declare a `TABLE Item` with no conflict. This is why `table_type`/`row_ref_type` need a `qualified_table` form: expressions could already reach another module's table through ordinary chained field access (`weapons.Item.ADD(...)` — §3.1's alias, then the table name, then any table operation), but a *type* position (a column, a parameter, a return type, a `let` annotation) had no way to say "a row reference into that specific module's table" until now:

```
import weapons
import consumables

FN repairAll(kit: &weapons.RepairKit, target: &weapons.Item) { ... }
```

One level of qualification is as deep as this ever needs to go — a module only ever reaches another module through its own local alias (§3.1), never by re-spelling a full `module_path`, so `weapons.Item` is already the fully-qualified form from the referencing module's point of view.

### 5.0 Function types

A `function_type` is the type of a function value — a reference to a specific, compiler-known `FN` or lambda, identified by its signature. Because every function is already a bare code pointer with no captures (§4.2.5), a function value is a compile-time-known address tagged with its signature — nothing is allocated or captured at the point of assignment:

```
FN isMinor(p: &Person) -> bool { return p.age < 18 }
FN isAdult(p: &Person) -> bool { return p.age >= 18 }

let pred: (&Person) -> bool = isMinor
```

A function-typed value can be reassigned to any named `FN` or lambda whose parameter and return types match exactly — no implicit conversion between different function types, same as everywhere else in the type system (§5.8).

Function types are not nilable: a function value is always a valid code address. `((&Person) -> bool)?` is a type error.

### 5.1 The reference model

Removing value references (`&int`) — see 5.1.1 below — leaves exactly two reference kinds, both spelled with identifiers rather than a shared ambiguous sigil doing double duty:

| Type                           | Storage                   | Copy semantics                                                                            |
| ------------------------------ | ------------------------- | ----------------------------------------------------------------------------------------- |
| Primitive (`int`, `bool`, ...) | Inline                    | Copy the value                                                                            |
| Table (`Person`)               | Pointer to sheet          | Copy the pointer (share the sheet)                                                        |
| Row reference (`&Person`)      | Row index (1–8 bytes)     | Copy the index (share the row)                                                            |
| Function ((`&T`) `-> R`)       | Compile-time code address | Copy the address — never allocated, never captures (§6.9)                                 |
| Array (`[T]`, `[N, T]`)        | Depends on `T`            | Copy the array (element-wise for primitives, pointer copy per element for row references) |
| Host type                      | Opaque                    | Host-defined                                                                              |

`Person` names the sheet; `&Person` names a reference to one of its rows. Both are references under the hood, but they refer to different things, and the type name makes that explicit at every use site.

The runtime representation of a `&T` reference is a row index, not a pointer — see the storage model document for details. The language guarantees only that a `&T` is comparable, may be `nil`, and is invalidated by `REMOVE` of the target row.

#### 5.1.1 No value references

There is no `&int`, `&string`, etc. Primitives are always copied. If a function needs to mutate a caller's data, it takes a row reference and writes a cell — there is no other way to achieve "output parameter" semantics, and none is needed.

### 5.2 Nilability of row references

**Every row-reference type (`&T`) is inherently nilable** — `nil` is an ordinary value of any `&T` type, the same way a null pointer is an ordinary value of a pointer type. This is a property `&T` already has, not a second type layered on top of it.

- `T.by<Column>(...)`-style lookups (`@primary`, §4.1.5) return `&T`, and are `nil` when nothing matches.
- A row reference stored in a cell becomes `nil` if the row it pointed to is removed (§7.6).
- `nil` is compared with `==`/`!=` (§6.8) and defaulted with `??` (§5.4).

Bare table types are never nilable.

**Dereferencing a `nil` row reference panics.** A program that wants to avoid the panic must check with `!= nil` before dereferencing. `&T` values do not require narrowing (§5.3); the user is responsible.

### 5.3 Nilability of non-reference types

**A type that may hold `nil` is written with a `?` suffix.** `T?` is a nilable `T`.

```
let x: int?     = nil
let y: string?  = "hello"
let z: [int]?   = nil
```

The suffix applies to primitives, host types, and arrays. The following are nilable types:

- `T?` for a primitive `T` (`int?`, `float?`, `bool?`, `char?`, `string?`, `unit?`).
- `T?` for a host type `T` (`SpriteRef?`).
- `T?` for an array type (`[int]?`, `[4, int]?`).

The following are type errors:

- `T?` for a bare table type: a table is a global; "the table is nil" is meaningless.
- `T?` for a function type: a function value is always defined.
- `T?` for `unit`: `unit` already means "no value"; `unit?` is redundant and forbidden.
- `T?` for a `&T`: `&T` is already nilable; `&T?` is accepted but the `?` is redundant.

A `?` written after a function type is a type error, but the parser's attachment of the `?` is different from what a reader might expect. In `(int) -> string?`, the `?` binds to the **return type** `string`, producing a function that returns a nilable `string`. There is no way to write "a nilable function type": the trailing `?` after `string` is always consumed by the return type's `type` production (which is `base_type [ '?' ]`), and a second `?` (as in `(int) -> string??`) attaches to the whole function type and is rejected by Sema. A function value is always a valid code address; nilability of a function value is meaningless.

This is the "old rule we are reusing". The `?` suffix is a suffix on the *innermost* type at the point it's parsed; for function types, the innermost type at the end is the return type, so that's where the `?` goes. The "nilable function type" the user might have wanted is not expressible, and Sema rejects attempts to express it.

**A `&T?` written explicitly is the same type as `&T`.** The parser accepts it; Sema silently treats it as `&T`. The `?` on a reference is a readability hint, not a distinct type.

**A nilable value must be narrowed before use.** §6.13 describes the narrowing rules. Reading a `T?` in a context that requires a `T` is a compile error unless the value has been narrowed.

```
let x: int? = lookup()

let y: int = x          -- error: x may be nil; narrow first
let z: int = x ?? 0     -- OK: `??` produces a non-nil value
if x != nil {
    let w: int = x      -- OK: x narrowed to non-nil inside the branch
}
```

**The `?` suffix binds to the innermost type.** For a dynamic array, `[int?]` is an array of nilable elements, and `[int]?` is a nilable array. Both are valid and distinct.

**For a fixed-size array**, the `?` inside the brackets makes the elements nilable, and the `?` after the brackets makes the array nilable:

- `[5, int]` — a fixed array of 5 non-nilable ints.
- `[5, int?]` — a fixed array of 5 nilable ints.
- `[5, int]?` — a nilable fixed array of 5 non-nilable ints.
- `[5, int?]?` — a nilable fixed array of 5 nilable ints.

All four are distinct types. `[5, int??]` is invalid (there is no second `?` inside the brackets); `[5, int]??` is invalid (the array can be nilable, but the nilability is not itself nilable).

### 5.4 The `??` operator

`??` is the null-coalescing operator:

```
expr ?? fallback
```

If `expr` (a nilable value) is `nil`, evaluate and return `fallback`. Otherwise return `expr` unchanged. The result is non-nil.

- LHS type: any nilable type (`T?` or `&T`).
- RHS type: the corresponding non-nil type (`T`).
- Result type: `T`.

```
let a: int?   = lookup()
let b: int    = a ?? 0           -- int
let c: &Item  = Item.byId(1) ?? fallbackItem   -- &Item (RHS may be nil if fallbackItem is nil, but that's the caller's choice)
```

There is no `if cond ?? a else b` ternary form — that reused `??` for an unrelated second meaning and is removed; use a plain `if` statement.

### 5.5 Arrays

- `[T]` — dynamic; grows and shrinks via `.ADD`/`.REMOVE` (§8).
- `[N, T]` — fixed-size, `N` a compile-time constant; supports indexing, `.COUNT()`, `.CONTAINS()`, and iteration, but not `.ADD`/`.REMOVE`.

Array literals are first-class expressions: `[1, 2, 3]`. An empty `[]` requires a type context to infer the element type.

**Arrays are values.** An assignment `let b: [int] = a` copies the array's contents. This is unlike row references, which share.

**Nested arrays are allowed.** `[[int]]` is a dynamic array of dynamic arrays of ints; `[[5, int]]` is a dynamic array of fixed arrays of 5 ints; `[5, [int]]` is a fixed array of 5 dynamic arrays of ints; `[5, [5, int]]` is a fixed array of 5 fixed arrays of 5 ints. The runtime stores the inner arrays' data in a shared flat buffer with per-outer-element offsets (a compressed-sparse-array layout).

### 5.6 Column views have no storable type

`T.column` (§7.4) produces a live view over a column's values, usable only inline in a `for` loop or an aggregation call (`.SUM()`, `.AVG()`). It cannot be assigned to a variable or passed as an argument — it is not a value of any type in this grammar, dynamic array included, because unlike an array it is a live view rather than a copy. To obtain a real, storable `[T]` copy of a column, call `.toArray()` on it.

### 5.7 Host types

A host type is declared with `TABLE X = host("name")` (§4.1.2). The script can hold and pass values of this type but not inspect or construct them. A host type may be nilable: `X?`.

### 5.8 Numeric literals and coercion

**No implicit coercion between two already-typed values.** `int + float` is a compile error; use an explicit conversion function (`toFloat(x) + y`, §11.1).

**Integer and float literals are untyped until context fixes them.** `42` is not pre-typed as `int32` and then rejected in a `uint` or `long` context — it adapts to whatever concrete numeric type the surrounding context requires:

```
let a: uint  = 42     -- OK, 42 adapts to uint
let b: long  = 42     -- OK, 42 adapts to long
let c: uint  = a + 1  -- OK, 1 adapts to uint to match a
let d: float = a      -- error: a is already uint32; no implicit coercion
```

A literal adapts to a nilable numeric type as well: `let x: int? = 42` is valid, with `42` becoming a non-nil `int?`.

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
                | range_expr           -- see §6.12
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

range_expr    ::= expr range_op expr [ range_op expr ]
range_op      ::= '..' | '..<'

unary_expr    ::= ( '-' | 'not' | '~' ) expr
binary_expr   ::= expr binary_op expr
binary_op     ::= '+' | '-' | '*' | '/' | '%' | '**'
                | '==' | '!=' | '<' | '<=' | '>' | '>='
                | 'and' | 'or'
                | '&' | '|' | '^' | '<<' | '>>'
                | '??'

paren_expr    ::= '(' expr ')'
```

### 6.1 Identifier, table access

`Person` names the sheet. `Person.ADD(...)`, `Person[i]`, `Person.column` are the sheet operations (§7). A bare `Person` used as a value is a reference to the sheet itself.

### 6.2 Field access

`p.name` reads or writes a cell of the row `p`. `p` must be a `&T` (a row reference); dereferencing a `nil` `&T` panics.

`math.sqrt` (where `math` is a module) accesses a module export. Sema classifies a field access as a cell access, a column view, or a module access based on the object's type.

### 6.3 Index

`container[index]`:

- `Person[i]` accesses row `i` of the `Person` sheet. Out of bounds panics.
- `arr[i]` accesses element `i` of an array. Out of bounds panics.

### 6.4 Call

`f(args)` calls a function. `callee` may be a named `FN`, a lambda, a table method (`Person.ADD`), or a module function (`math.sqrt`). No currying.

### 6.5 Array literal

`[1, 2, 3]` produces an array. The element type is inferred from context. `[]` requires a type context.

### 6.6 Lambda expression

A lambda is `(params) -> expr`. See §6.9.

### 6.7 Parenthesized expression

`(expr)` groups an expression. The AST keeps the paren node for source reconstruction.

### 6.8 Equality and comparison

| Operators         | Valid operand types                  | Meaning                                                                               |
| ----------------- | ------------------------------------ | ------------------------------------------------------------------------------------- |
| `==` `!=`         | primitives                           | value equality                                                                        |
| `==` `!=`         | `&T` (including against `nil`)       | identity — do both sides refer to the same row (or is one/both `nil`)                 |
| `==` `!=`         | `T?` and `nil`                       | nil-check                                                                             |
| `==` `!=`         | two `T?` values of the same `T`      | both `nil`, or both non-nil and equal                                                 |
| `==` `!=`         | `[T]` / `[N, T]`                     | structural: same length, each element equal (element-wise identity for `&T` elements) |
| `==` `!=`         | function types                       | identity — same underlying `FN`/lambda or not                                         |
| `<` `<=` `>` `>=` | numeric primitives, `string`, `char` | ordering (lexicographic for `string`)                                                 |

Comparing two bare table-typed expressions (`Person == Person`) is a compile error — there is exactly one sheet per table name, so the comparison is always trivially true and carries no information. Ordering operators are not defined for `&T`, table, or array types.

**Comparing a `T?` to a non-nil `T` is a compile error.** The RHS must be `T?` or `nil`. This prevents the common bug of comparing a possibly-nil value to a real value.

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

Because a function value is a compile-time-known address rather than something constructed at runtime, a bare function name is also a valid `const_expr` (§4.1.1c) — a fixed table can hold behavior directly:

```
FIXED TABLE StateHandler {
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

`..`/`..<` (range operators, §6.12) are not in this table — they don't compose with the arithmetic/logical operators above. A range's operands are restricted to literals, arithmetic on literals, and `T.Member` references (the same shape as `const_expr`, §4.1.1c), so there's no case where range precedence needs to interact with, say, `+` or `and`.

### 6.12 Range expressions

A range is `lo..hi` (inclusive) or `lo..<hi` (exclusive). **A range is not a first-class value** — Sema rejects it anywhere a real type is expected (`let x: int = 0..10` is an error; there's no range type in §5), because it cannot be assigned to a variable, passed to a function, or used in arithmetic. It's legal in exactly two positions, both enforced by Sema rather than the grammar:

- as the iterable of a `for` loop (§12.3);
- as a `case_value` in a `switch` case (§12.2).

Both bounds must be the same integer type — like any other numeric context (§5.8), untyped integer literals adapt to whatever concrete type the position requires.

A range used as a `for` iterable may additionally carry a step, written with a second range operator: `lo..hi..step` or `lo..<hi..step`. A step of zero is a compile error; a negative step counts down. A range used as a `switch` case value may not carry a step, and both bounds must be compile-time constants (§12.2).

In a `for` loop over a range, the loop variable takes the values `lo`, `lo+step`, `lo+2*step`, ... up to and including `hi` (`..`) or up to but not including `hi` (`..<`). The default step is `1`.

```
for i: int in 0..<10 { ... }        -- 0, 1, 2, ... 9
for i: int in 0..10..2 { ... }      -- 0, 2, 4, 6, 8, 10
for i: int in 10..0..-1 { ... }     -- 10, 9, 8, ... 0
```

A range's `lo` and `hi` (and `step`, if present) are stored on the `RangeExprAST`; the step is `nullptr` when no step was written.

### 6.13 Narrowing of nilable values

A `T?` value must be narrowed before being used where a `T` is expected. Narrowing happens when the compiler can prove the value is not `nil` at the use site:

**By nil-check.** A `!= nil` comparison narrows the value inside the branch:

```
let x: int? = lookup()
if x != nil {
    let y: int = x          -- OK: x narrowed to int inside the branch
}
```

A `== nil` comparison narrows the value inside the else branch:

```
if x == nil {
    -- x is nil here; using x as an int would be an error
} else {
    let y: int = x          -- OK: x narrowed to int
}
```

**By `??`.** The result of `??` is non-nil:

```
let y: int = x ?? 0         -- OK: y is int
```

**By `&&` chaining.** Narrowing propagates through `&&`:

```
if x != nil && y != nil {
    let sum: int = x + y    -- OK: both narrowed
}
```

**By early return.** If a `nil` branch returns, the code after the `if` sees the value as narrowed:

```
FN compute() -> int {
    let x: int? = lookup()
    if x == nil {
        return 0
    }
    return x                -- OK: x narrowed to int
}
```

**Narrowing applies to `T?` values only.** A `&T` value does not require narrowing; dereferencing a `nil` `&T` panics at runtime. A program that wants to avoid the panic checks with `!= nil` before dereferencing, but the check is not required by the type system.

**Narrowing is per-expression, not per-variable.** A narrowed `x` in one branch does not narrow `x` in another. A narrowed `x` at one use site does not narrow it at another. The compiler tracks the narrowed state through the control flow within a single function body; it does not track it across function calls.

---

## 7. Table operations

### 7.1 Sheet-level operations

| Operation             | Result       | Notes                                                                                                                                                   |
| --------------------- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `T.ADD(args...)`      | `&T`         | Append a row. Not available on fixed or `@readonly` tables.                                                                                             |
| `T.REMOVE(i)`         | `unit`       | Remove row `i`. Not available on fixed or `@readonly` tables. Rows after `i` are not shifted; slot `i` is reused by a future `ADD`.                     |
| `T[i]`                | `&T`         | Row at slot `i`. **Panics** if `i` is out of bounds or refers to a removed slot.                                                                        |
| `T.at(i)`             | `&T`         | Row at slot `i`; returns `nil` instead of panicking if `i` is out of bounds or dead.                                                                    |
| `T.COUNT()`           | `uint`       | Number of live rows.                                                                                                                                    |
| `T.FIND(pred)`        | `T`          | A live view of rows matching `pred: (&T) -> bool` — a lambda or a named `FN` (§6.9). No copy; invalidated by a subsequent `REMOVE` on the parent table. |
| `T.by<Column>(value)` | `&T`         | Generated when a column has `@primary` (e.g. `byId`); O(1) via the primary index; `nil` if no row matches.                                              |
| `T.column`            | (view, §5.6) | Iterable/aggregable view over one column's values across all rows.                                                                                      |
| `T.Member`            | `&T`         | Fixed-table sugar: resolves to the row whose first `string` column equals `"Member"`, at compile time.                                                  |

**Iteration order is slot order.** Rows are visited in slot order; a slot reused by a later `ADD` appears at its slot's position, not at the end. A program that needs insertion order must maintain it explicitly.

### 7.2 Row-level operations

| Operation          | Result | Notes                                                                                 |
| ------------------ | ------ | ------------------------------------------------------------------------------------- |
| `r.column`         | value  | Read a cell.                                                                          |
| `r.column = value` | `unit` | Write a cell. Not available if the table is `@readonly` or the column is `@readonly`. |

### 7.3 Panic vs. nil — which operations do which

**Panics** (bugs, not recoverable in-language — see §10): `T[i]` out of bounds or on a dead slot, a duplicate `@unique`/`@primary` value on `ADD`, and dereferencing a `nil` row reference (`"attempt to access a nil value"`).

**Returns `nil`** (an expected, checkable absence): `T.at(i)` out of range or on a dead slot, `T.by<Column>(value)` with no match, a cell whose referenced row was removed.

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

A slot reused by a subsequent `ADD` gets a new row; any `&T` value that referred to the *old* occupant of the slot is now `nil` (its generation counter doesn't match), so the reference is safe even though the slot is occupied.

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

### 7.8 Array-typed columns

A column whose type is `[T]` holds one array per row. The runtime stores the arrays' data in a shared flat buffer with per-row offsets (a compressed-sparse-array layout). Reading `r.col[i]` looks up the row's `(offset, length)` and reads from the shared buffer.

**A row's array cell may be reassigned.** `r.col = [1, 2, 3]` replaces the row's array. The runtime stores the new array in the shared buffer (at a new offset), and the row's cell is updated to point at it. Old data may leave holes; a table marked `@packed` cannot have holes, so `@packed` forbids reassigning an array cell after initialization.

**Array-typed columns may be nested:** `[[int]]` holds one array-of-arrays per row. The layout is the same, with an extra level of offsets.

---

## 8. Array operations

| Operation         | Result | Available on                  |
| ----------------- | ------ | ----------------------------- |
| `arr[i]`          | `T`    | `[T]`, `[N, T]`               |
| `arr.ADD(x)`      | `unit` | `[T]` only                    |
| `arr.REMOVE(i)`   | `unit` | `[T]` only                    |
| `arr.COUNT()`     | `uint` | `[T]`, `[N, T]`               |
| `arr.CONTAINS(x)` | `bool` | `[T]`, `[N, T]` (linear scan) |

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

**Looping to modify every element needs the index form, not the value form.** For primitive element types, `for x: T in arr` binds a *copy* of each element, and — since loop bindings are `const` (§12.3) — writing to `x` isn't just pointless, it's rejected outright:

```
for x: int in scores {
    x = x * 2    -- error: x is a loop binding, which is const
}
```

That compile error is a deliberate improvement over the alternative: a mutable-by-value loop binding would let this compile and silently do nothing to `scores`, which is a much worse failure mode than catching it before the program ever runs.

To mutate the array in place, iterate with the index and assign back through it:

```
-- Doubles every score in place
for i: uint, x: int in scores {
    scores[i] = x * 2
}
```

This is the array equivalent of the table pattern in §4.1.6: a table's `for r: &Person in Person` binding is a row *reference*, so writing `r.age = ...` mutates the underlying row directly — no index needed. An array of primitives has no such reference to hand out (§5.1.1 — primitives are always copied), so the index is how you get back to the slot you want to change.

**Array `REMOVE` does shift later elements down.** This is unlike a table's `REMOVE`, which reuses slots. The distinction: a table's rows are referenced by `&T` values, which need stable slots; an array's elements are copied, so shifting is invisible to anyone who isn't holding an index.

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

Attributes never change what the parser reads for the declaration that follows — they're metadata Sema interprets, not syntax that reshapes the declaration. The full set is listed in §4.1.4, §4.1.5, and §4.2.6.

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

**`waitUntil` takes its predicate and argument separately rather than as a closed-over lambda.** A lambda can only see its own parameters and module-level declarations (§6.9) — it cannot capture a local like a request handle you just created. Passing the value in explicitly (`waitUntil(isDown, Key.Space)`, `waitUntil(isLoaded, req)`) keeps the no-capture rule intact everywhere, including here: `pred` is an ordinary no-capture function or lambda, and `arg` is just another local the compiler already has to keep alive across the pause.

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

- **Panics** — programmer bugs: out-of-bounds `T[i]`, a `@unique`/`@primary` violation, dereferencing `nil`. Not recoverable inside the script.
- **`nil`** — a legitimately absent result: `T.at(i)`, `T.by<Column>(...)`, a cell whose row was removed, a `T?` value. Checked with `== nil` / `!= nil` or defaulted with `??`.

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
statement     ::= var_decl
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
                | 'waitForRequest' '(' expr ')'          -- see §9.2; valid only inside a @sequence body (§9.2.2)

if_stmt       ::= 'if' expr block [ 'else' ( block | if_stmt ) ]
switch_stmt   ::= 'switch' expr '{' { case_clause } default_clause '}'
case_clause   ::= 'case' case_value { ',' case_value } ':' block
case_value    ::= expr
default_clause ::= 'default' ':' block

while_stmt    ::= [ label ] 'while' expr block
for_stmt      ::= [ label ] 'for' binding { ',' binding } 'in' expr block
label         ::= IDENTIFIER ':'
binding       ::= IDENTIFIER ':' type | '_'

expr_stmt     ::= expr
```

An `assign_stmt` is a statement only — there is no assignment form in the `expr` grammar (§6), so `let y: int = (x = 5)` is a syntax error; assignment must be its own statement, never nested inside an expression.

A `suspend_stmt` node parses wherever any statement is allowed — the parser doesn't special-case its position. Sema is what actually restricts it: it reports a diagnostic if a `suspend_stmt` appears outside a function tagged `@sequence` (§9.2.2).

### 12.1 Compound assignment

`lvalue op= expr` desugars to `lvalue = lvalue op expr`. Valid for every numeric and bitwise binary operator; not defined for `&T`, table, or array lvalues. Not defined for `??` (there is no `??=`).

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

The check only applies when the subject's type is a specific fixed table — it doesn't run for a growing table (which has no fixed member list to check against) or for a `switch` over an ordinary primitive value.

**The match is by reference identity.** When the subject is a `&T` for a fixed table `T`, each `case_value` is a `&T` reference, typically the fixed-table sugar `T.Member` (e.g. `Direction.North`). The match compares the subject to the case value using `==` on `&T`, which is reference identity (§6.8). The fixed table's number of columns is irrelevant — a case names a row, not a set of field values. Even a table with multiple columns is matched by reference; Sema's exhaustiveness check verifies that every row of the fixed table appears as a case value, not that any field-level equality holds.

A `case` may list several values separated by commas; the case matches if the subject equals any one of them:

```
switch d {
    case Direction.North, Direction.South: { println("vertical") }
    case Direction.East, Direction.West:   { println("horizontal") }
    default: { }
}
```

Each `case_value` must be a constant expression — a literal, a fixed-table member (`Direction.North`), a small arithmetic combination of literals, or a range (§6.12, without a step, both bounds compile-time constants). Sema enforces the constant-ness.

Because a growing table's row references are not compile-time constants, a `switch` over a growing table is not expressible: its case values cannot be constant expressions. A `switch` is intended for fixed tables and primitive values.

### 12.3 `for` bindings

A `for` loop takes one or two bindings; how many, and what they mean, depends on the iterable:

| Iterable                   | One binding            | Two bindings                   |
| -------------------------- | ---------------------- | ------------------------------ |
| Range (§6.12)              | the counter            | —                              |
| Table / `FIND` view (§7.7) | a row reference (`&T`) | index (`uint`) + row reference |
| Column view (§7.4)         | the value              | —                              |
| Array (§8.2)               | the element            | index (`uint`) + element       |

```
for i: int in 0..<10 { ... }             -- a range: i is the counter
for r: &Person in Person { ... }         -- a table: r is a row reference
for i: uint, r: &Person in Person { ... } -- a table, indexed
for v: int in Person.age { ... }         -- a column view: v is the value
for x: int in scores { ... }             -- an array: x is the element
for i: uint, x: int in scores { ... }    -- an array, indexed
```

A binding may be `_` to discard its value; a discarded binding takes no type annotation:

```
for _, x: int in scores { ... }          -- values only
for i: uint, _ in scores { ... }         -- indices only
```

**Loop bindings are `const` within the body** — assigning to one directly is a compile error. Shadow with a `let` inside the body if a mutable local copy is genuinely wanted:

```
for x: int in scores {
    let doubled: int = x * 2    -- OK: a new local, not a write to the binding
    println(doubled)
}
```

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

`break` and `continue` without a label affect only the nearest enclosing loop, exactly as before — a label is purely opt-in. `continue label` jumps to the next iteration of the labeled loop rather than the innermost one.

A label is its own small namespace: it never collides with a variable, function, or table name, and `break`/`continue`'s optional identifier is resolved only against enclosing labels, not against ordinary identifiers in scope.

### 12.5 Local declarations

`var_decl` may appear inside a block; its scope is the block. `FN` and `TABLE` may only appear at module (top) level — no nested declarations of either kind.

---

## 13. What this grammar deliberately excludes

- Traits, `DEF`, `REQUIRE`, `OpKind`, user-defined operator overloading.
- A `fn`/`cls` distinction, general closures, currying.
- `async`, `await`, `spawn`, `Deferred<T>`, threads, locks — see §13.1. (`start`/`wait`/`waitFrames`/`waitUntil`/`waitForEvent`/`waitForRequest` are adopted, but only as the narrow sequence primitive in §9.2, not a general concurrency system.)
- Generics.
- Value references (`&int`) — primitives are always copied.
- `@default(expr)` on a column (§4.1.5) — a default value is domain logic, expressed as an ordinary wrapper function, not storage metadata.
- `@optional` on a column (§4.1.5) — nilability is a type-level property, expressed with the `?` suffix (§5.3).

### 13.1 Why sequences (§9.2), not general concurrency

Every async-shaped need identified so far, apart from one, reduces to a synchronous call from the script's point of view once the callback mechanism (§9.1) exists:

- **Input** — an `@on(EventKind.KeyDown)` callback, called once per event.
- **Network** — sending is a fire-and-forget `host(...)` call; receiving is an `@on(EventKind.NetworkMessage)` callback when a response lands.

The one case that doesn't fit a callback is **sequencing** work across time inside one logical unit — "wait 2 seconds, then do X," for cutscenes and scripted dialogue — because that requires *pausing partway through a function's own body and resuming it later*, which a callback (which always returns control to the engine immediately) can't express. §9.2 answers that directly with `@sequence` and its suspend points plus `start` to launch one, rather than deferring it.

This is still a narrow, single-purpose addition, not general concurrency: `@sequence` functions cannot call each other directly (§9.2.4), the runtime advances every active sequence cooperatively and in a fixed, deterministic order once per tick (§9.2.5), and there is no preemption, no shared-memory data race, and nothing resembling a thread or a lock anywhere in the model.

---

## 14. Remaining open items (deferred, not blocking)

1. Direct sequence-to-sequence composition (today, a `@sequence` composes another only via `start` + `waitUntil(isDone, handle)`, §9.2.5 — a nested-composition form, if ever needed, is a bigger compiler change and deliberately not attempted yet).
2. Registry-scoping tooling for Tier 2 mods (the *policy* — one-way dependency, Tier-1-only registration — is settled in §3.4; the concrete host-side API for defining a mod's registry view is not part of this document).
3. `T.compact()` for growing tables, to reclaim dead slots after many `REMOVE`s. v1 accepts the memory waste of unused slots; a future `compact()` would shift live rows and rewrite all references.
4. Non-nil reference types (e.g. a type that means "a `&T` that is never `nil`"), which would let a constructor like `ADD` return a value that the caller can use without a nil-check. v1 has no such type; the caller checks.

---

*This document consolidates the full design conversation: the tables/functions/variables redesign, the sheet-vs-shape resolution, the attribute system, the `??`/nilability rework (including `?` as a type suffix for non-reference types), lambdas and first-class function types, `switch` (with fixed-table exhaustiveness warnings), compound assignment, array operations, numeric coercion rules, the `@on(...)` event/callback mechanism, the `@sequence` suspension primitive, the host/standard-library split, and the Tier 1/Tier 2 loading model.*