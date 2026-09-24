# Lucid Grammar

## Version 0.1 — Draft

---

## Table of Contents

**Part I — Foundations**
- [Introduction](#introduction)
- [Design Principles](#design-principles)
- [Execution Model](#execution-model)
- [Lexical Structure](#lexical-structure)
- [Program Structure](#program-structure)

**Part II — Frames**
- [The Type Frame — `TYPE`](#the-type-frame-type)
- [The Value Frame — `FN`, `const`, `let`](#the-value-frame-fn-const-let)
- [The Trait Frame — `trait`, `satisfy`, `REQUIRE`](#the-trait-frame-trait-satisfy-require)
- [The Behavior Frame — `DEF`](#the-behavior-frame-def)

**Part III — Types**
- [Primitive Types](#primitive-types)
- [Structs](#structs)
- [Enums](#enums)
- [Generics](#generics)
- [Function Types](#function-types)
- [Array and Sequence Types](#array-and-sequence-types)
- [References and `Weak<T>`](#references-and-weakt)
- [Nullable, Fallible, and Combined Types](#nullable-fallible-and-combined-types)

**Part IV — Statements and Control Flow**
- [Statements](#statements)
- [`if` / `else`](#if-else)
- [Loops](#loops)
- [`switch` and Pattern Matching](#switch-and-pattern-matching)
- [Pattern Matching (Future)](#pattern-matching-future)

**Part V — Expressions**
- [Expression Forms](#expression-forms)
- [Literals](#literals-1)
- [Identifier Expressions](#identifier-expressions)
- [Struct and Array Literals](#struct-and-array-literals)
- [Field Access](#field-access-1)
- [Indexing and Slicing](#indexing-and-slicing)
- [Module Access](#module-access)
- [Calls](#calls)
- [Operators](#operators)
- [Precedence and Associativity](#precedence-and-associativity)
- [`??` Fallback](#fallback)
- [Pipeline](#pipeline)
- [Function Literals](#function-literals)
- [Range Expressions](#range-expressions)
- [Parenthesized Expressions](#parenthesized-expressions)

**Part VI — Behavior System**
- [The `DEF` Table](#the-def-table)
- [The Standard `OpKind` Set](#the-standard-opkind-set)
- [Overload Resolution](#overload-resolution)
- [`satisfy` and Constraint Checking](#satisfy-and-constraint-checking)
- [The `Stringable` Trait](#the-stringable-trait-1)
- [`OpKind` Resolution](#opkind-resolution)

**Part VII — Concurrency**
- [The Fiber Model](#the-fiber-model)
- [`async`, `spawn`, `start`, `await`](#async-spawn-start-await)
- [`Deferred<T>` — Linear Value Rules](#deferredt-linear-value-rules)
- [Cancellation](#cancellation)
- [Fibers and `spawn` Without a Handle](#fibers-and-spawn-without-a-handle)
- [Concurrency and the Standard Library](#concurrency-and-the-standard-library)
- [Restrictions Summary](#restrictions-summary)

**Part VIII — Standard Library**
- [The Core Scripts](#the-core-scripts)
- [Primitive Types and Their Operations](#primitive-types-and-their-operations)
- [The Trait Catalog](#the-trait-catalog)
- [`Option<T>`, `Fallible<T>`, `Both<T>`](#optiont-falliblet-botht)
- [`toStr`](#tostr)
- [`print` and `println`](#print-and-println)
- [`error` and `warn`](#error-and-warn)
- [`Map<K, V>`](#mapk-v)
- [Array Operations](#array-operations)
- [String Operations](#string-operations)
- [Math Operations](#math-operations)
- [`Weak<T>` and Cycle Handling](#weakt-and-cycle-handling)
- [`Deferred<T>` and Concurrency Support](#deferredt-and-concurrency-support)
- [`core.fn` — Function Composition](#corefn-function-composition)
- [`core.io` — Console and File I/O](#coreio-console-and-file-io)

**Part IX — Interop and Directives**
- [The Foreign Function Interface](#the-foreign-function-interface)
- [Attributes](#attributes)
- [The `#builtin` Registry](#the-builtin-registry)
- [The `#hostFn` Boundary](#the-hostfn-boundary)
- [Interop Summary](#interop-summary)

**Part X — Reference**
- [Operator Precedence](#operator-precedence)
- [The Boot Set](#the-boot-set)
- [Open Items](#open-items)
- [Summary of the Language](#summary-of-the-language)
- [Quick Reference Tables](#quick-reference-tables)

---


## Introduction

Lucid is an embedded scripting language for a C++ game engine. It runs as bytecode inside a VM that is linked directly into the host engine, sharing its lifetime and its thread. A native ahead-of-time compilation path is planned but is not part of this specification.

The design has four commitments that shape every section below:

**The core script is the grammar.** The parser recognizes a small, fixed vocabulary — the frames, the punctuation, and a short list of keywords — and nothing else. Every type, operator, function, trait, and constant is declared in a *core script*: a Lucid source file loaded before any user code. Where another language hard-codes a primitive, Lucid declares it. Where another language adds a keyword, Lucid adds a declaration. The parser's job is to recognize shapes; the core script's job is to fill them.

**One VM thread.** Script code runs on a single thread, driven by a cooperative scheduler. Fibers suspend only at defined points, so script data cannot be raced by construction. True parallelism lives on the C++ side, reached through an `#host` boundary that returns a `Deferred<T>` the script can await.

**Explicit over implicit.** There is no method dispatch, no type inference, no function overloading, and no type erasure. A type is always written; a shape is always declared; a conversion is always named. The two implicit operations in the whole language are `fn → cls` coercion (a bare function pointer can be wrapped as a closure) and the `Weak<T>` upgrade (a weak reference can be promoted to a strong one under a null check). Both are called out in the sections that define them.

**Value semantics by default; references are opt-in.** `T` copies. `&T` aliases, and the referent is refcounted. `Weak<T>` is a reference that does not keep its referent alive. Cycles of strong references leak unless broken with `Weak<T>`; the compiler warns on the obvious cases.

The rest of this document specifies the language those commitments produce.

---

## Design Principles

A few principles are worth stating explicitly, because they explain many of the individual rules below.

**One keyword per concept.** Two concepts with two keywords are easier to teach than one keyword with two meanings. This is why `TYPE` and `trait` are different keywords, why `const` and `let` are both available, and why `DEF` and `REQUIRE` do not share a spelling.

**Keyword case signals category.** Lowercase keywords name things: `const`, `let`, `import`, `trait`, `satisfy`, `struct`, `enum`. Uppercase keywords bind to the host or declare behavior: `TYPE`, `FN`, `DEF`, `REQUIRE`. A reader scanning raw text can tell at a glance which kind of declaration a keyword begins.

**Declarations produce values; statements produce effects.** Every declaration form has a *target* slot, filled by one of a fixed set of shapes (`#host(...)`, an identifier, a struct body, an expression, a block). A statement is a value consumed for its effect, a binding introduced, or a control-flow jump. The two categories do not overlap.

**The parser does not know the language.** The parser knows frames, content markers, statement keywords, punctuation, and literals. It does not know `int`, `+`, `Map`, `println`, `Numeric`, or any other name from the standard library. Those are declared in core scripts, and the compiler looks them up the same way it looks up a user's own declarations.

**Below the grammar, above the host.** Between the parser and the C++ engine sits the VM: a bytecode interpreter, a refcount table, a fiber scheduler, and a small set of compiler-recognized operations that emit code. This document specifies the grammar those layers consume. It does not specify the bytecode, the refcount layout, or the host ABI.

---

## Execution Model

A Lucid program is a single entry-point file, `main.luc`, plus the transitive closure of everything it imports. Nothing else exists for that program. A declaration in a file that is never (transitively) imported does not participate in type resolution, operator resolution, or trait conformance.

The compiler loads core scripts first, in a fixed order: the base module (`core`), then the standard-library modules (`core.map`, `core.array`, `core.string`, `core.math`, `core.io`) in dependency order, then the user's program. Core scripts use the same frames as user code; nothing about them is special at the syntax level, but their declarations are registered before user code so that user code can refer to them.

Execution proceeds in three phases:

1. **Load and validate.** All imports are resolved, all core scripts are parsed, and the core scripts' declarations are registered. This phase has no user-visible effect.

2. **Compile.** Each user module is parsed, semantic analysis runs, and bytecode is generated. Compilation is per-module for the parse and Sema phases, and whole-program for operator resolution and trait conformance checks.

3. **Run.** The VM executes the program's entry point. Every function the program calls is either a Lucid function (running as bytecode), a core-script function (also bytecode), a `#builtin` handler (compiler-emitted code), or an `#host` function (a C++ function registered with the engine).

A program that fails any phase before run does not execute. Diagnostics are collected and reported together at the end of compile; the program does not run on a failed compile.

---

## Lexical Structure

### Source form

Source files are UTF-8. A file is a sequence of declarations, with optional comments and whitespace between them. There is no preprocessor, no include, and no macro expansion.

### Keywords

**Frame keywords.**

| Lowercase                                | Uppercase                   |
| ---------------------------------------- | --------------------------- |
| `const` `let` `import` `trait` `satisfy` | `TYPE` `FN` `DEF` `REQUIRE` |

**Content markers.**

```
struct  enum  fn  cls  as  Self
```

**Statement keywords.**

```
if  else  for  while  do  switch  case  default
break  continue  return
async  spawn  start  await  all  any
```

**Literals.**

```
nil  err  true  false
```

No keyword may be used as an identifier. The keyword set is closed; adding to it is a change to the boot set (see [`switch` and Pattern Matching](#switch-and-pattern-matching), Boot Set).

### Identifiers

```
IDENTIFIER ::= LETTER { LETTER | DIGIT | '_' }
LETTER     ::= 'a'..'z' | 'A'..'Z' | '_'
DIGIT      ::= '0'..'9'
```

Identifiers are case-sensitive. A name beginning with an underscore is legal and unremarkable; the language reserves no prefix.

### Literals

```
INT_LIT    ::= DIGIT+
             | '0x' HEX_DIGIT+
             | '0b' BIN_DIGIT+
             | '0o' OCT_DIGIT+

FLOAT_LIT  ::= DIGIT+ '.' DIGIT+ [ ('e'|'E') ['+'|'-'] DIGIT+ ]

STRING_LIT ::= '"' { STRING_CHAR | interpolation } '"'
             | '"""' { ANY_CHAR } '"""'

CHAR_LIT   ::= '\'' ( CHAR_CHAR | ESCAPE ) '\''

BOOL_LIT   ::= 'true' | 'false'

STRING_CHAR ::= ANY_CHAR_EXCEPT('"', '\', NEWLINE)
ESCAPE      ::= '\' ('n'|'t'|'r'|'\'|'"'|'0')

interpolation ::= '\(' expr ')'
```

A `"..."` string is a normal string: escapes are processed, newlines are not allowed, and `\(expr)` interpolates an expression. A `"""..."""` string is raw: no escape processing, no interpolation, and the content may span multiple lines. The only sequence a raw string cannot contain is `"""` itself.

`nil` and `err` are boot literals. Their types (`Option<T>` and `Fallible<T>`) are declared in the core script, but the tokens themselves are recognized by the lexer so that `x == nil` and `x == err` parse without any script loaded.

### Comments

```
line_comment  ::= '--' { ANY_CHAR } NEWLINE
block_comment ::= '/-' { ANY_CHAR | block_comment } '-/'    -- nestable
doc_comment   ::= '/--' { ' -' ANY_CHAR NEWLINE } '--/'
```

A line comment runs to the end of the line. A block comment is delimited by `/-` and `-/` and may nest. A doc comment is a block comment whose content is attached to the following declaration. Doc comments are attached to declarations only; they do not attach to statements or expressions.

### Punctuation

```
( ) { } [ ]
, ; :: .
.. ->
= ? !
+ - * / % **
== != < <= > >=
and or not
& | ^ ~ << >>
|> ??
@ #
```

Every operator token is syntactic. Its meaning for a given pair of operand types is determined by a `DEF` declaration (see [The Type Frame — `TYPE`](#the-type-frame-type)); if no `DEF` exists for the operand types, the operation is a compile error, not a parse error.

Some tokens have context-dependent forms:

- `&` is a reference marker in type position (`&T`) and the bitwise AND operator in expression position.
- `?` and `!` are postfix type suffixes in type position (`T?`, `T!`). In expression position, `!` is the argument-pack marker inside a pipeline step (see [Program Structure](#program-structure)); `?` does not appear in expression position.
- `..` is the range operator. The exclusive form `..<` is lexed as `..` followed by `<` and parsed as a single range operator.
- `::` is module-qualified access; `.` is struct field access.
- `@` and `#` are deliberately not interchangeable, even though both precede a bracketed or parenthesized name. `@[...]` is always *metadata about* a declaration that already has its own identity — `@[export]`, `@[deprecated]`, `@[foreign("C")]` — and never appears as a value in its own right. `#host(...)`, `#native(...)`, `#builtin(...)`, and the bare `#hostFn` marker are always the value filling a target slot — the implementation itself, resolved directly by the compiler/VM rather than through `DEF`'s overload table. A reader can tell which is meant from the sigil alone, without knowing what follows it.

### Whitespace and separators

Whitespace is not significant except as a token separator. Commas and semicolons are optional in many positions and may be used freely for readability; where they are required, the sections that define the construct say so.

### Line and column

Every token carries a source location: file, line, column. Diagnostics use these locations. Doc comments, attributes, and declaration keywords are attached to the declaration they precede.

---

## Program Structure

```
program        ::= { import_decl } { top_level_decl }

top_level_decl ::= type_decl
                 | value_decl
                 | trait_decl
                 | satisfy_decl
                 | def_decl

import_decl    ::= 'import' module_path [ 'as' IDENTIFIER ]
module_path    ::= IDENTIFIER { '.' IDENTIFIER }
```

The top level of a file contains only declarations. There are no top-level statements; there is no implicit script-level execution. Every program has exactly one entry point, declared with the `@[export]` attribute:

```lucid
@[export] const main (args [*]string) -> int = {
    return 0;
};
```

### Module identity

A file is a module. The file's path, relative to the package root, is the module's identity. There is no `module` or `namespace` declaration inside a file; the file *is* the module, and the path *is* the name.

An `import` declaration brings another module into scope. The module path is resolved against the package root; the resolution rules are the loader's concern, not the grammar's.

### Exports

A declaration is visible outside its file only if marked `@[export]`. Everything else is private to the file. There is no separate `pub`/`private` syntax; the attribute is the whole mechanism.

### Import closure

A program is `main.luc` plus the transitive closure of everything it imports. A declaration in a file that is never (transitively) imported does not exist for that program, for any purpose — not for name resolution, not for operator resolution, not for trait conformance.

This has a consequence worth stating: a `DEF` for an operator is only visible if the module that declares it is reachable from `main`. A `satisfy` block is only registered if its module is reachable. And a trait's conformance is only checkable if the trait itself is reachable.

### Declaration order

Declarations in a module are processed in the order they appear, with one exception: `import` declarations must precede all other top-level declarations. A declaration may refer to another declaration in the same module that appears later in the file; the compiler does two passes over each module — a name-registration pass, then a resolution pass.

### Attribute placement

An attribute list precedes the declaration it modifies. At the top level, it precedes a `type_decl`, `value_decl`, `trait_decl`, `satisfy_decl`, or `def_decl`. Inside a struct or trait body, it precedes a field or clause. Attributes on local declarations are permitted (see [Enums](#enums)).

### Examples

Minimal program:

```lucid
@[export] const main (args [*]string) -> int = {
    return 0;
};
```

Program with imports and a helper:

```lucid
import core.io as io
import core.math as math

const double fn (x int) -> int = {
    return x * 2;
};

@[export] const main (args [*]string) -> int = {
    io::println("doubled: " ++ toStr(double(21)));
    return 0;
};
```

Program with a type and an operator:

```lucid
struct Vec2 {
    x float;
    y float;
}

DEF BINARY_OP '+' (a Vec2, b Vec2) -> Vec2 = {
    return Vec2 { x = a.x + b.x, y = a.y + b.y };
};

@[export] const main (args [*]string) -> int = {
    let v Vec2 = Vec2 { x = 1.0, y = 2.0 } + Vec2 { x = 3.0, y = 4.0 };
    println(toStr(v));
    return 0;
};
```

---

## The Type Frame — `TYPE`

The `TYPE` frame declares a named type. Every type name in the language — primitive, host-bound, user-defined — comes from a `TYPE` declaration in some script. The frame is the same in all cases; what varies is the *target*, which says what kind of type the name refers to.

```
type_decl    ::= 'TYPE' IDENTIFIER [ generic_params ] '=' type_target ';'

type_target  ::= '#host' '(' IDENTIFIER ')'
               | '#native' '(' IDENTIFIER ')'
               | '#builtin' '(' IDENTIFIER ')'
               | IDENTIFIER
               | 'struct' '{' { struct_field } '}'
               | 'enum'   '{' { enum_variant } '}'

generic_params ::= '<' generic_param { ',' generic_param } '>'
generic_param  ::= IDENTIFIER [ ':' IDENTIFIER { '+' IDENTIFIER } ]

struct_field ::= { attribute_list } [ 'const' ] IDENTIFIER type [ '=' expr ] ';'

enum_variant ::= { attribute_list } IDENTIFIER '=' INT_LIT ';'
               | { attribute_list } IDENTIFIER '(' type ')' ';'
```

### The six targets

**`#host(Ident)`** — binds the Lucid name to a C++ type registered with the engine. The host type's layout and behavior are defined on the C++ side; Lucid code can use the name in type positions, but cannot inspect its layout.

**`#native(Ident)`** — binds the name to a VM-native type. Core scripts only. A user script using `#native` is a compile error; the VM's internals are not part of the user-facing type system.

**`#builtin(Ident)`** — binds the name to a compiler-known type constructor. Used for types like `Option<T>` and `Fallible<T>` whose representation the compiler emits directly. The identifier in the parentheses names the builtin handler (see [Nullable, Fallible, and Combined Types](#nullable-fallible-and-combined-types), the `#builtin` registry).

**`Ident`** — an alias. The name refers to another Lucid type, resolved by looking up the identifier in the type namespace. Alias chains are followed until a terminal target (host, native, builtin, struct, enum) is reached. A cycle is a compile error.

**`struct { ... }`** — a compound value type. The body declares fields; see [Structs](#structs) for the field grammar and [Structs](#structs) for trait conformance.

**`enum { ... }`** — a tagged type. The body declares variants; see [Enums](#enums).

### Surface sugar for `struct` and `enum`

The declarations

```lucid
struct Point { x float; y float; }
enum Direction { North = 0; East = 1; }
```

are sugar for

```lucid
TYPE Point = struct { x float; y float; }
TYPE Direction = enum { North = 0; East = 1; }
```

The parser expands the sugar immediately and produces the same AST in both cases. The sugar exists because the standalone forms are more readable for the common case; the expansion exists because downstream passes should reason about one construct.

### Generic parameters

A `TYPE` declaration may be generic:

```lucid
TYPE Map<K, V> = #host(LucidMap)
TYPE Result<T, E> = enum { Ok(T); Err(E); }
```

The parameters are in scope in the target's body (for `struct` and `enum`) and are substituted at each use site. The rules for generic parameters are in [Function Types](#function-types).

A generic parameter may carry constraints:

```lucid
TYPE Cache<K : Eq, V : Stringable> = #host(LucidCache)
```

`+` joins multiple constraints on one parameter; `,` separates parameters.

### Aliases

An alias introduces a second name for an existing type:

```lucid
TYPE int = #host(int)
TYPE km = int
TYPE Metres = km
```

`km` and `Metres` are aliases to `int`. Aliases are transparent: a value of type `Metres` is a value of type `int`, and vice versa. There is no wrapper, no conversion, and no distinct identity.

Aliases are resolved eagerly — the compiler walks the chain to its terminal target during name resolution, and downstream passes see only the terminal target. A chain that does not terminate (a cycle) is a compile error with a diagnostic naming the cycle.

### `@[opaque]` and the type frame

`@[opaque]` is a field attribute, not a type attribute. It appears inside a `struct` body on individual fields, not on the `TYPE` declaration itself. Its semantics are defined in [Structs](#structs).

### Examples

Primitive declaration (in the core script):

```lucid
TYPE int    = #host(int)
TYPE float  = #host(float)
TYPE string = #host(string)
TYPE bool   = #host(bool)
```

Host-backed container (in the core script):

```lucid
TYPE Map<K, V> = #host(LucidMap)
TYPE Deferred<T> = #host(LucidDeferred)
TYPE Weak<T> = #host(LucidWeak)
```

User-defined struct with trait conformance:

```lucid
struct Vec2 : Vector2 {
    x float;
    y float;
}
```

Payload-carrying enum:

```lucid
enum JsonValue {
    Num(float);
    Str(string);
    Arr([*]JsonValue);
    Obj([*]KeyValue);
}
```

---

## The Value Frame — `FN`, `const`, `let`

The value frame declares a name bound to a value or a callable. Three keywords select the binding's mutability and the general shape of its target:

- **`FN`** — for host-backed declarations. The target is `#host`, `#native`, or `#builtin`. No Lucid body.
- **`const`** — for immutable bindings. The target may be any value expression, a Lucid body, or a host-bound declaration.
- **`let`** — for mutable bindings. Same targets as `const`.

```
value_decl ::= ('FN' | 'const' | 'let') IDENTIFIER [ generic_params ]
               [ signature ] '=' value_target ';'

signature  ::= '(' [ param_list ] ')' '->' type

value_target ::= '#host' '(' IDENTIFIER ')'
               | '#native' '(' IDENTIFIER ')'
               | '#builtin' '(' IDENTIFIER ')'
               | IDENTIFIER
               | expr
               | block

param_list ::= param { ',' param }
param      ::= [ 'const' ] IDENTIFIER [ '...' ] type
```

### When to use which keyword

The keywords are conventions, not hard rules — the parser accepts any keyword with any target, and the compiler rejects mismatches with a clear diagnostic.

- **`FN`** for declarations whose implementation lives on the C++ side or in the VM: `FN map_new<K, V> () -> Map<K, V> = #host(map_new);`
- **`const`** for immutable bindings: `const pi float = 3.14159;` and `const double (x int) -> int = { return x * 2; };`
- **`let`** for mutable bindings: `let counter int = 0;`

The distinction is only about reassignability and the conventional target. A `const` function binding whose body is a Lucid block is a perfectly ordinary function; an `FN` binding to `#host` is a foreign declaration. Neither is more "function-y" than the other.

### Signatures

A signature is a parameter list and a return type. The return type may be omitted for declarations that produce no value (`@[foreign("C")] FN glClear (mask uint32) = #host(glClear);` — the return type defaults to `unit`).

Function-typed bindings (`let f fn (int) -> int = ...`) use the full function-type grammar from [Array and Sequence Types](#array-and-sequence-types) for their signature. The `fn`/`cls` markers are written in the signature; they are not implied by the `let`/`const` keyword.

### Parameters

A parameter is a name, a type, and two optional modifiers:

- **`const`** before the name — a read-only reference parameter. The function sees the caller's original value but cannot modify it.
- **`...`** between the name and the type — a variadic parameter. Collects zero or more trailing arguments into a `[*]T`.

```lucid
const sum fn (nums ...int) -> int = {
    let total int = 0;
    for _, n int in nums { total = total + n; }
    return total;
};

const describe fn (const v Vec2) -> string = {
    return "(" ++ toStr(v.x) ++ ", " ++ toStr(v.y) ++ ")";
};
```

A variadic parameter must be the last parameter in its group. A parameter without `const` or `...` is passed by value (a copy).

### Currying

A declaration's parameter list may be written as multiple groups separated by `->`:

```lucid
const add fn (a int) -> fn (b int) -> int = {
    return (b int) -> int { return a + b; };
};
```

The leading group introduces the declaration's parameters. Subsequent groups describe the return type, which is itself a function type. Each subsequent group's parameters are introduced by the nested function literal in the body.

Adjacent groups (`fn (a int) fn (b int) -> int`) are desugared to arrow-separated groups by the parser. The two spellings produce the same AST.

### Generic declarations

A declaration with a non-empty `generic_params` list is generic:

```lucid
const identity<T> fn (v T) -> T = { return v; };
TYPE Box<T> = struct { value T; };
```

**A generic function must be `const`.** A `let`-bound generic function is a Sema error:

```
error: a generic function must be declared 'const'
   = help: a generic function is a definition, not a reassignable
     value; no expression form produces a generic-family value
```

The reasoning: `let` asks for a reassignability capability, but no expression form produces a generic family as a value, so a `let`-bound generic declaration has nothing it could ever be reassigned to. `const` describes what the declaration already is.

### The generic `TYPE`/`FN` split

A generic `TYPE` declares a type family. `TYPE Box<T> = struct { value T; }` declares `Box` as a family, and `Box<int>`, `Box<string>` are members.

A generic `FN` or `const` declares a function family. `const identity<T> fn (v T) -> T = { ... }` declares `identity` as a family, and `identity<int>`, `identity<string>` are members.

A bare family name is not a value. `identity` names a family; `identity<int>` names a member, which is a value. This is enforced by Sema ([Function Types](#function-types)).

### Value targets

The target slot accepts five shapes:

**`#host(Ident)`** — the implementation is a C++ function registered with the engine.

**`#native(Ident)`** — the implementation is a VM opcode. Core scripts only.

**`#builtin(Ident)`** — the implementation is a compiler-emitted operation. See [Nullable, Fallible, and Combined Types](#nullable-fallible-and-combined-types).

**`Ident`** — the binding refers to another existing declaration. The name is resolved and the binding becomes an alias for it.

**`expr`** — any expression producing a value of the declared type. This includes `func_literal` (an anonymous function) and `call_expr` (a call returning a value).

**`block`** — a Lucid function body. The block is desugared to an `AnonFuncExprAST` whose signature is taken from the declaration's `signature`. For a curried declaration, the block is the outermost function's body; nested function literals in the block supply the inner groups.

### Examples

Host-bound function:

```lucid
FN write (s string) = #host(host_write);
```

Lucid function:

```lucid
const double fn (x int) -> int = {
    return x * 2;
};
```

Immutable data:

```lucid
const pi float = 3.14159;
const greeting string = "hello";
```

Mutable data:

```lucid
let counter int = 0;
counter = counter + 1;
```

Function-typed binding:

```lucid
const apply fn (f fn (int) -> int, x int) -> int = {
    return f(x);
};

let square fn (int) -> int = (x int) -> int { return x * x; };
```

Curried:

```lucid
const clamp fn (lo int) -> fn (hi int) -> fn (v int) -> int = {
    return (hi int) -> fn (v int) -> int {
        return (v int) -> int {
            if v < lo { return lo; }
            if v > hi { return hi; }
            return v;
        };
    };
};
```

Generic:

```lucid
const first<T> fn (items [_]T) -> T? = {
    if items.length == 0 { return nil; }
    return items[0];
};
```

---

## The Trait Frame — `trait`, `satisfy`, `REQUIRE`

A trait declares a set of requirements. A `trait` block introduces the trait; a `satisfy` block asserts that a specific type meets the requirements; a `REQUIRE` clause names an operation the trait demands.

```
trait_decl   ::= 'trait' IDENTIFIER [ generic_params ]
                 [ ':' IDENTIFIER { ',' IDENTIFIER } ]
                 '{' { trait_clause } '}'

trait_clause ::= 'FIELD' IDENTIFIER type ';'
               | 'REQUIRE' op_kind STRING_LIT
                 '(' param_list ')' '->' type ';'

satisfy_decl ::= 'satisfy' trait_application 'for' type '{' { def_decl } '}'

trait_application ::= IDENTIFIER [ '<' type_arg { ',' type_arg } '>' ]
type_arg ::= type | INT_LIT
```

### One construct, two clause kinds

A `trait` declares requirements. Two clause kinds are available:

**`FIELD name type;`** — the type must have a field with this name and type. A trait with `FIELD` clauses is **struct-flavored**: only types with fields can satisfy it.

**`REQUIRE op_kind "symbol" (params) -> type;`** — the type must provide an operation of the given kind and signature. A trait with `REQUIRE` clauses is **operation-flavored**: any type that provides the operations can satisfy it, including primitives.

A trait may have both kinds. Its flavor is determined by which clauses it has:

| Clauses        | Flavor             | Satisfiable by                                    |
| -------------- | ------------------ | ------------------------------------------------- |
| `FIELD` only   | struct-flavored    | structs                                           |
| `REQUIRE` only | operation-flavored | any type                                          |
| Both           | mixed              | structs (with the required fields and operations) |

### Trait bodies

A trait body is a sequence of clauses. Each clause begins with `FIELD` or `REQUIRE`. There is no other declaration form inside a trait body.

```lucid
trait Numeric {
    REQUIRE BINARY_OP '+' (self Self, rhs Self) -> Self;
    REQUIRE BINARY_OP '-' (self Self, rhs Self) -> Self;
    REQUIRE BINARY_OP '*' (self Self, rhs Self) -> Self;
    REQUIRE BINARY_OP '/' (self Self, rhs Self) -> Self;
}

trait Vector2 {
    FIELD x float;
    FIELD y float;
}

trait Vector2Arithmetic {
    FIELD x float;
    FIELD y float;
    REQUIRE BINARY_OP '+' (self Self, rhs Self) -> Self;
    REQUIRE BINARY_OP '*' (self Self, s float) -> Self;
}
```

`Self` in a clause refers to the concrete type being satisfied. It is substituted at each `satisfy` site.

### Trait inheritance

A trait may list parent traits after a colon:

```lucid
trait Ord : Eq {
    REQUIRE BINARY_OP '<' (self Self, rhs Self) -> bool;
}
```

A type satisfying `Ord` must also satisfy `Eq`. The parent traits are checked first; a type that fails `Eq` cannot satisfy `Ord`. A type satisfying `Ord` is also considered to satisfy `Eq` for constraint-checking purposes.

`+` joins multiple parents: `trait Numeric : Add + Sub + Mul + Div {}`.

### Generic traits

A trait may carry generic parameters:

```lucid
trait Container<T> {
    FIELD value T;
    FIELD count uint;
}

trait Convertible<From, To> {
    REQUIRE CALL 'to' (self Self, v From) -> To;
}
```

A constraint site uses a concrete instantiation: `<T : Container<int>>`. The compiler substitutes the arguments at the constraint site.

### The `satisfy` block

A `satisfy` block asserts that a specific type satisfies a specific trait:

```lucid
satisfy Numeric for Vec2 {
    DEF BINARY_OP '+' (a Vec2, b Vec2) -> Vec2 = { return Vec2 { x = a.x + b.x, y = a.y + b.y }; };
    DEF BINARY_OP '-' (a Vec2, b Vec2) -> Vec2 = { return Vec2 { x = a.x - b.x, y = a.y - b.y }; };
    DEF BINARY_OP '*' (a Vec2, b Vec2) -> Vec2 = { return Vec2 { x = a.x * b.x, y = a.y * b.y }; };
    DEF BINARY_OP '/' (a Vec2, b Vec2) -> Vec2 = { return Vec2 { x = a.x / b.x, y = a.y / b.y }; };
}
```

The block contains only `DEF` declarations (see [The Behavior Frame — `DEF`](#the-behavior-frame-def)). Each `DEF` provides an implementation for one of the trait's `REQUIRE` clauses.

**Field conformance is implicit.** A trait's `FIELD` clauses are checked against the type's own field declarations; the `satisfy` block does not list them. For a struct-flavored trait:

```lucid
struct Vec2 { x float; y float; }

satisfy Vector2 for Vec2 {
    -- empty: the fields x and y are checked against the struct's own declaration
}
```

An empty block is legal for a field-only trait. The compiler verifies that the struct's fields match the trait's `FIELD` clauses; a mismatch is a compile error at the `satisfy` block, pointing at the field that differs.

### Missing and extra members

**Missing required member.** If a `satisfy` block does not provide a `DEF` for one of the trait's `REQUIRE` clauses, it is a compile error:

```
error: 'satisfy Numeric for Vec2' does not provide a definition for
       BINARY_OP '/' (a Vec2, b Vec2) -> Vec2
   = note: 'Numeric' requires this operation; the satisfy block
     must provide it
```

**Extra member.** A `DEF` in a `satisfy` block that the trait does not require is allowed. The `DEF` is a normal declaration in the block's scope; it is visible wherever a `DEF` at that position would be visible. The block is both an assertion of conformance and a grouping of related declarations.

### Generic `satisfy` blocks

A `satisfy` block may be generic, covering all instantiations of a generic trait in one block:

```lucid
trait Container<T> {
    FIELD value T;
    FIELD count uint;
}

struct Box<T> { value T; count uint; }

satisfy Container<T> for Box<T> {
    -- fields value: T and count: uint are checked against Box<T>
}
```

The parameter list `<T>` is shared between the trait application and the type. Every parameter that appears in either must be listed; a parameter that appears in neither is a compile error. When the compiler needs to check `<U : Container<int>>` at a call site, it looks up the `satisfy Container<T> for Box<T>` block, substitutes `T := int`, and verifies the substituted field types.

### `Self` and self-reference

`Self` in a trait clause refers to the concrete type being satisfied. A `FIELD` clause may use `Self` in the field's type:

```lucid
trait Node {
    FIELD value int;
    FIELD next Self?;
}
```

The clause `FIELD next Self?` requires the type to have a field `next` whose type is a nullable reference to itself. When `PlayerNode` is satisfied against `Node`, the compiler substitutes `Self := PlayerNode` and checks that `PlayerNode` has a field `next` of type `PlayerNode?`.

A **non-nullable** self-referential field is a compile error at the trait declaration:

```
error: non-nullable self-reference creates infinite size
   --> traits.luc:2:11
    |
  2 |     FIELD next Self;
    |           ^^^^
    |
   = help: add '?' to make the field nullable: FIELD next Self?;
```

The rule is the same as for struct fields: a type cannot contain itself inline. The nullable form is legal and lowers to a pointer.

### Trait conformance checking order

Conformance is checked in a pre-pass over every module reachable from `main`, before any constraint sites are verified. The order is:

1. Every `trait` declaration is registered with its clauses.
2. Every `satisfy` block is checked against its trait: every `REQUIRE` clause must have a matching `DEF`, and every `FIELD` clause must match a field of the type.
3. Every `trait X : A, B` inheritance is checked: a type satisfying `X` must also satisfy `A` and `B`.
4. Constraint sites (`<T : TraitName>`) are checked against the registered `satisfy` blocks.

A `satisfy` block that is well-formed but whose trait is never used is still checked in step 2. A constraint site that refers to a trait with no matching `satisfy` block for the concrete type is a compile error in step 4.

### Examples

Operation-only trait satisfied by a primitive:

```lucid
trait Numeric {
    REQUIRE BINARY_OP '+' (self Self, rhs Self) -> Self;
    REQUIRE BINARY_OP '-' (self Self, rhs Self) -> Self;
}

satisfy Numeric for int {
    DEF BINARY_OP '+' (a int, b int) -> int = #native(add_i32);
    DEF BINARY_OP '-' (a int, b int) -> int = #native(sub_i32);
}
```

Field-only trait satisfied by a struct:

```lucid
trait Named {
    FIELD name string;
}

struct Player { name string; health int; }

satisfy Named for Player {
    -- empty: name field checked against Player's own declaration
}
```

Generic trait with generic satisfy:

```lucid
trait Container<T> {
    FIELD value T;
    FIELD count uint;
}

struct Box<T> { value T; count uint; }

satisfy Container<T> for Box<T> { }

const first<T> fn (c Container<T>) -> T? = {
    if c.count == 0 { return nil; }
    return c.value;
};
```

---

## The Behavior Frame — `DEF`

A `DEF` declares an operator or call fact: "this operation exists for these operand types." Every operator in the language — arithmetic, comparison, indexing, named-call dispatch — is a `DEF`.

```
def_decl ::= 'DEF' op_kind STRING_LIT [ generic_params ]
             '(' param_list ')' '->' type '=' def_impl ';'

op_kind  ::= IDENTIFIER
def_impl ::= '#host' '(' IDENTIFIER ')'
           | '#native' '(' IDENTIFIER ')'
           | '#builtin' '(' IDENTIFIER ')'
           | IDENTIFIER
           | block
```

### `op_kind` is an identifier

The `op_kind` slot is an identifier, not a keyword. It is resolved by Sema against `OpKind` values declared in the core script:

```lucid
const BINARY_OP  OpKind = registerOpKind("BINARY_OP");
const UNARY_OP   OpKind = registerOpKind("UNARY_OP");
const INDEX_GET  OpKind = registerOpKind("INDEX_GET");
const INDEX_SET  OpKind = registerOpKind("INDEX_SET");
const CALL       OpKind = registerOpKind("CALL");
```

The parser recognizes an identifier in the `op_kind` slot and defers resolution to Sema. A `DEF` whose `op_kind` does not resolve to a registered `OpKind` value is a compile error:

```
error: undefined operation kind 'BINARY_OPP'
   = help: did you mean 'BINARY_OP'?
```

The set of valid op kinds is open in principle — a core script can call `registerOpKind` to add new ones — but in practice the standard set is fixed by the core scripts and adding to it is a design change.

### The symbol

The string literal after the op kind names the specific operation:

```lucid
DEF BINARY_OP '+' (...)
DEF BINARY_OP '==' (...)
DEF UNARY_OP  'not' (...)
DEF INDEX_GET (...)
DEF INDEX_SET (...)
DEF CALL 'toStr' (...)
```

For `BINARY_OP` and `UNARY_OP`, the symbol is the operator's spelling. For `INDEX_GET` and `INDEX_SET`, there is no symbol — the operation is implicit in the op kind, and the string is empty or omitted. For `CALL`, the symbol is a name: `CALL 'toStr'` declares a named-call operation, dispatched by name.

### Implementation

The implementation slot accepts:

**`#host(Ident)`** — the operation is implemented by a C++ function.

**`#native(Ident)`** — the operation is a VM opcode.

**`#builtin(Ident)`** — the operation is a compiler-emitted handler.

**`Ident`** — the operation is another existing function, aliased.

**`block`** — the operation has a Lucid body. The block's signature is taken from the `DEF` header; the parser synthesizes the anonymous function's signature and does not require it to be repeated.

```lucid
DEF BINARY_OP '+' (a Vec2, b Vec2) -> Vec2 = {
    return Vec2 { x = a.x + b.x, y = a.y + b.y };
};
```

The block is desugared to an `AnonFuncExprAST` whose parameter list is copied from the `DEF` header. The user never writes the signature twice.

### Generic `DEF`

A `DEF` may be generic:

```lucid
DEF BINARY_OP '!=' <T : Eq> (a T, b T) -> bool = {
    return not (a == b);
};
```

The generic parameters are in scope in the implementation. Overload resolution prefers a concrete `DEF` over a generic one ([Primitive Types](#primitive-types)).

### `DEF`s outside `satisfy` blocks

A `DEF` may appear at the top level of a module, outside any `satisfy` block. Such a `DEF` is a *fact*: the operation exists for these types, but the type is not asserted to satisfy any trait that requires it. A `DEF` is usable the moment it is declared; whether it also satisfies a trait is a separate question answered by a `satisfy` block.

```lucid
struct Money { cents int; }

-- fact: Money + Money is defined, but Money does not formally satisfy
-- any trait that requires '+'
DEF BINARY_OP '+' (a Money, b Money) -> Money = {
    return Money { cents = a.cents + b.cents };
};
```

If a later `satisfy Add for Money { DEF BINARY_OP '+' ... }` is added, the trait constraint `<T : Add>` can be satisfied by `Money`. Without it, `Money` can be used with `+` but cannot be used where `Add` is required.

### `DEF`s inside `satisfy` blocks

A `DEF` inside a `satisfy` block is both a fact and part of the block's assertion. It declares the operation, and the block asserts the trait is satisfied. `DEF`s inside a `satisfy` block are lexically scoped to the block, but the operations they declare are visible program-wide (subject to the module's export rules).

### `INDEX_GET` and `INDEX_SET`

The two index op kinds have a fixed shape:

```
DEF INDEX_GET (container, key) -> value;
DEF INDEX_SET (container, key, value) -> unit;
```

The first parameter is the container, the second is the key (or index), and `INDEX_SET` takes a third parameter which is the value to store. `INDEX_SET` returns `unit`.

`INDEX_GET` may return a nullable value (`V?`) when the key might not be present:

```lucid
DEF INDEX_GET (m &Map<K, V>, k K) -> V? = #builtin(map_get);
DEF INDEX_SET (m &Map<K, V>, k K, v V) = #builtin(map_set);
```

The user calls them with the ordinary index syntax:

```lucid
let v int? = m["alice"];
m["bob"] = 20;
```

### `CALL`

The `CALL` op kind declares a named-call operation. Its first parameter is the receiver, and its remaining parameters are the call's arguments:

```lucid
DEF CALL 'toStr' (v int) -> string = #builtin(int_to_str);
DEF CALL 'toStr' (v string) -> string = { return v; };
DEF CALL 'toStr' (v Vec2) -> string = { return "(" ++ toStr(v.x) ++ ", " ++ toStr(v.y) ++ ")"; };
```

A call like `toStr(42)` resolves through the `CALL` table, keyed on the argument's type.

`CALL` is what makes user-defined stringification work: `toStr` is a `CALL` op, `Vec2` provides its own implementation, and the compiler dispatches to it via the `CALL` table.

### Examples

Concrete binary operator:

```lucid
DEF BINARY_OP '+' (a Vec2, b Vec2) -> Vec2 = {
    return Vec2 { x = a.x + b.x, y = a.y + b.y };
};
```

Generic derived operator:

```lucid
DEF BINARY_OP '!=' <T : Eq> (a T, b T) -> bool = {
    return not (a == b);
};
```

Index operations for a custom container:

```lucid
struct Grid { cells [*]float; width uint; }

DEF INDEX_GET (g &Grid, i uint) -> float = {
    return g.cells[i];
};

DEF INDEX_SET (g &Grid, i uint, v float) = {
    g.cells[i] = v;
};
```

Named call:

```lucid
DEF CALL 'describe' (v int) -> string = {
    return "int: " ++ toStr(v);
};

DEF CALL 'describe' (v string) -> string = {
    return "string: " ++ v;
};
```

---

## Primitive Types

The primitive types are declared in the core script. The parser does not know their names; it resolves them the same way it resolves any other type name.

```
TYPE bool   = #host(bool)
TYPE char   = #host(char)
TYPE string = #host(string)

TYPE byte   = #host(int8)
TYPE short  = #host(int16)
TYPE int    = #host(int32)
TYPE long   = #host(int64)

TYPE ubyte  = #host(uint8)
TYPE ushort = #host(uint16)
TYPE uint   = #host(uint32)
TYPE ulong  = #host(uint64)

TYPE float  = #host(float)
TYPE double = #host(double)
```

The `int`/`uint` names are aliases for the 32-bit variants on the reference platform. The sized names (`int8`, `uint64`, etc.) are also declared in the core script for cases where a specific width is required:

```lucid
TYPE int8   = byte
TYPE int16  = short
TYPE int32  = int
TYPE int64  = long
TYPE uint8  = ubyte
TYPE uint16 = ushort
TYPE uint32 = uint
TYPE uint64 = ulong
```

The primitive set is a starting catalog, not a closed list. A future core script can add a new primitive by declaring another `TYPE X = #host(...)`, and user code can use it the same way it uses `int`.

### Primitive operations

Each primitive's operations are declared as `DEF`s in the core script. For `int`:

```lucid
DEF BINARY_OP '+' (a int, b int) -> int = #native(add_i32);
DEF BINARY_OP '-' (a int, b int) -> int = #native(sub_i32);
DEF BINARY_OP '*' (a int, b int) -> int = #native(mul_i32);
DEF BINARY_OP '/' (a int, b int) -> int = #native(div_i32);
DEF BINARY_OP '%' (a int, b int) -> int = #native(rem_i32);
DEF BINARY_OP '==' (a int, b int) -> bool = #native(eq_i32);
DEF BINARY_OP '!=' (a int, b int) -> bool = #native(ne_i32);
DEF BINARY_OP '<' (a int, b int) -> bool = #native(lt_i32);
DEF BINARY_OP '<=' (a int, b int) -> bool = #native(le_i32);
DEF BINARY_OP '>' (a int, b int) -> bool = #native(gt_i32);
DEF BINARY_OP '>=' (a int, b int) -> bool = #native(ge_i32);
```

The same set exists for each numeric type. `bool` has the logical operators; `string` has concatenation and comparison; `char` has comparison and conversion.

### Numeric literals

A numeric literal is untyped at the lexer level. The parser produces a `LiteralExprAST` with the raw lexeme, and the type checker assigns a concrete type based on the surrounding declared type. `42` in a position expecting `int` becomes `int`; the same literal in a position expecting `uint64` becomes `uint64`.

When no target type is available, the literal defaults to `int` for integers and `double` for floats. A literal that does not fit its target type is a compile error.

### The `unit` type

A function that produces no value has return type `unit`. `unit` is a host-bound type with a single value; it is not `void` because it participates in the type system as a real type. `write` returns `unit`:

```lucid
FN write (s string) = #host(host_write);
```

The omitted return type is sugar for `-> unit`.

---

## Structs

A struct is a compound value type. It is declared with `struct X { ... }` (sugar for `TYPE X = struct { ... }`), and it may carry generic parameters and trait conformance.

```
struct_decl  ::= 'struct' IDENTIFIER [ generic_params ]
                 [ ':' IDENTIFIER { ',' IDENTIFIER } ]
                 '{' { struct_member } '}'

struct_member ::= struct_field | static_fn_decl

struct_field ::= { attribute_list } [ 'const' ] IDENTIFIER type
                 [ '=' field_default ] ';'

field_default ::= expr | block

static_fn_decl ::= 'static' IDENTIFIER '(' param_list ')' '->' type '=' block
```

### Fields

A field is a name, a type, and an optional default:

```lucid
struct Point {
    x float;
    y float;
}
```

A field declared `const` cannot be reassigned after construction, even when the containing value is `let`:

```lucid
struct Counter {
    const step int = 1;
    total      int;
}
```

`const` fields may have defaults; the default is used when the field is omitted from a literal, and the value may be overridden at the literal site. Once construction finishes, the field is fixed.

### Field defaults

A field default is an expression evaluated when a literal omits the field. The default may be any expression that produces a value of the field's type, including a `block` for function-typed fields:

```lucid
struct Logger {
    sink cls (string) -> unit = { return (msg string) -> unit { write(msg); }; };
}
```

When the default is a block and the field's type is a function type, the parser synthesizes an `AnonFuncExprAST` whose signature is the field's type. The block is the function body.

### The implicit `self` for block defaults

A function-typed field's block default may reference the struct's own fields unqualified:

```lucid
struct Point {
    x float;
    y float;
    const describe fn () -> string = {
        return "(" ++ toStr(x) ++ ", " ++ toStr(y) ++ ")";
    };
}
```

Here `x` and `y` resolve to `Point`'s own fields. The compiler synthesizes an implicit `self: &Point` parameter for the block, and `x`/`y` are lowered to `self.x`/`self.y`. This is the only place in the language where field access is implicit.

An external override of a block-default field must declare `self` explicitly:

```lucid
const customDescribe fn (self &Point) -> string = {
    return "custom point";
};

let p Point = Point { x = 1.0, y = 2.0, describe = customDescribe };
```

### `@[opaque]` fields

A field marked `@[opaque]` cannot be initialized, read, or assigned from Lucid source:

```lucid
struct Map<K, V> {
    @[opaque] handle Handle;
}
```

Rules:

- The field cannot appear in a struct literal.
- The field cannot be read (`m.handle`).
- The field cannot be assigned (`m.handle = ...`).
- The field is skipped by `toStr`'s struct fallback.
- The field cannot satisfy a trait's `FIELD` clause of the same name.
- The field still occupies layout space.
- A struct with only `@[opaque]` fields has no literal form; it must be constructed by an `FN`.

The last rule is what makes `Map` constructible only via `map_new`. It is a consequence of the first rule, not a separate rule.

### Static members

A struct may declare static functions:

```lucid
struct Vec2 {
    x float;
    y float;

    static zero () -> Vec2 = {
        return Vec2 { x = 0.0, y = 0.0 };
    };

    static fromAngle (theta float) -> Vec2 = {
        return Vec2 { x = cos(theta), y = sin(theta) };
    };
}
```

Static members are accessed with `::`:

```lucid
let origin Vec2 = Vec2::zero();
let unit   Vec2 = Vec2::fromAngle(0.0);
```

A static member has no receiver and no per-instance storage. It is a plain function that happens to be namespaced under the struct. It cannot refer to the struct's fields without an explicit instance parameter.

### Trait conformance

A struct may list traits it satisfies:

```lucid
struct Vec2 : Vector2, Eq {
    x float;
    y float;
}
```

The traits are checked at the struct's declaration. Field-only traits are checked implicitly against the struct's fields; operation-only traits require a `satisfy` block or a `DEF` that matches the required operations. The conformance list is a shorthand for a `satisfy` block; both are equivalent, and either may be used.

### Recursive fields

A struct field whose type, after substitution, recursively contains the struct must be written with `?`:

```lucid
struct Node {
    value int;
    next  Node?;
}
```

The `?` is what makes the field nullable, and it is also what tells the compiler to lower the field as a pointer. A non-nullable self-referential field is a compile error:

```
error: non-nullable self-reference creates infinite size
   --> main.luc:2:11
    |
  2 |     next Node;
    |          ^^^^
    |
   = help: add '?' to make the field nullable: next Node?;
```

A self-referential field that is part of a larger recursive structure is detected the same way: the compiler walks the field's type after substitution and rejects any path that returns to the struct without a `?` in between.

### Struct literals

A struct value is constructed with a literal:

```lucid
let p Point = Point { x = 1.0, y = 2.0 };
let q Point = Point { x = 3.0 };    -- y takes its default
```

Field order in the literal is free. Fields with defaults may be omitted. A field without a default must be provided. A struct with only `@[opaque]` fields cannot be constructed with a literal.

A generic struct's literal names the instantiation:

```lucid
let b Box<int> = Box<int> { value = 42 };
```

### Field access

A field is accessed with `.`:

```lucid
let px float = p.x;
p.x = 5.0;    -- if p is let and x is not const
```

A field of a `const` binding cannot be reassigned. A `const` field of a `let` binding also cannot be reassigned. Both rules are enforced at the assignment site.

---

## Enums

An enum is a tagged type with a fixed set of named variants. It is declared with `enum X { ... }` (sugar for `TYPE X = enum { ... }`).

```
enum_decl     ::= { attribute_list } 'enum' IDENTIFIER [ ':' int_type ]
                  '{' { enum_variant } '}'

enum_variant  ::= { attribute_list } IDENTIFIER '=' INT_LIT ';'
                | { attribute_list } IDENTIFIER '(' type ')' ';'
```

### Integer variants

A variant with an explicit integer value:

```lucid
enum Direction {
    North = 0;
    East  = 1;
    South = 2;
    West  = 3;
}
```

The variant's value is required; there is no auto-increment. An enum whose variants are all integer-valued lowers to a bare integer of the backing type.

### Payload variants

A variant that carries a value:

```lucid
enum JsonValue {
    Num(float);
    Str(string);
    Arr([*]JsonValue);
    Obj([*]KeyValue);
}
```

The variant carries a value of the parenthesized type. An enum with any payload variant lowers to `{ tag: int, payload: byte[] }`, where `tag` identifies which variant is active and `payload` is a byte array sized to hold the largest variant.

### Mixed variants

A single enum may mix both forms:

```lucid
enum Token {
    Eof      = 0;
    Ident(string);
    Number(float);
    Operator = 4;
}
```

An enum with any payload variant is a payload enum for every purpose in this section — mixing costs nothing: an integer variant like `Eof` simply carries no payload, and occupies a tag value like every other variant. Mixing is also allowed together with an explicit backing type ([The backing type](#the-backing-type)); the backing type still names the tag, not the payload, regardless of how many variants are payload-less.

### The backing type

An enum may declare an explicit backing integer type with `:` after the name:

```lucid
enum Status : int32 {
    Ok       = 200;
    NotFound = 404;
    Error    = 500;
}

enum JsonValue : int8 {
    Num(float);
    Str(string);
    Arr([*]JsonValue);
}
```

The backing type must be an integer primitive, but it means one of two different things depending on the kind of enum, because the two kinds of enum have different runtime shapes:

- **Integer-only enum.** The backing type is the enum's storage — the enum *is* an integer of that type. An integer variant's value must fit the backing type; a value that does not fit is a compile error.
- **Payload-carrying enum.** The backing type is the **tag's** integer type only. The tag is the small integer that identifies which variant is active; the backing type bounds how many variants the tag can distinguish (an `int8` tag supports up to 256 variants). It does **not** bound the payload. The payload is sized independently, to hold the largest variant, regardless of the backing type.

A payload variant's storage is sized to hold its declared type. The backing type never constrains that size: `enum : int8` with a payload variant carrying a 1 KB struct has a 1-byte tag and a 1 KB payload — total size is `1 + 1024` bytes plus alignment. The backing type's width only ever affects the tag.

The two readings share one syntactic rule — the backing type names an integer type — and the distinction is visible where a reader is already looking: an enum body with only `Variant = N;` forms is an integer enum (backing type = storage); a body with any `Variant(T);` form is a payload enum (backing type = tag).

**Default.** If no backing type is given: an integer enum defaults to `int` (32-bit signed), matching an ordinary integer literal's default type. A payload enum defaults to the smallest unsigned integer that holds the variant count — `uint8` for up to 256 variants, `uint16` for up to 65,536, and so on. The tag is unsigned because a variant index is never negative; this is an implementation detail, invisible to user code, since a tag is never read back as a number.

### Variant access

An integer variant is read with `.`:

```lucid
let d Direction = Direction.North;
```

A payload variant is constructed with `(value)`:

```lucid
let v JsonValue = JsonValue.Num(3.14);
let s JsonValue = JsonValue.Str("hello");
```

A variant is never an l-value. `Direction.North = Direction.East` is a compile error.

### `switch` and pattern binding

An enum is dispatched with `switch`:

```lucid
switch v {
    case JsonValue.Num(n): { println("number: " ++ toStr(n)); }
    case JsonValue.Str(s): { println("string: " ++ s); }
    case JsonValue.Arr(a): { println("array: " ++ toStr(array_len(a))); }
    case JsonValue.Obj(o): { println("object: " ++ toStr(array_len(o))); }
}
```

The binding `(n)`, `(s)`, `(a)`, `(o)` introduces the payload value in the case body. The payload's type is the variant's declared type. An integer variant is matched without a binding: `case Direction.North: { ... }`.

Exhaustiveness: a `switch` on an enum without a `default` must cover every variant. A missing variant is a compile error naming the variant that was not covered.

### Integer enums and the type system

An integer enum is a distinct type from its backing integer. `Direction.North` is not an `int`; it is a `Direction`. The conversion between them is explicit:

```lucid
let d Direction = Direction.North;
let n int = enumToInt(d);       -- explicit conversion (a core-script function)
let d2 Direction = intToEnum<Direction>(0);    -- explicit conversion
```

`enumToInt` and `intToEnum` are declared in the core script. `intToEnum` returns an optional: `Direction?`, because not every integer is a valid variant.

---

## Generics

A declaration may carry generic parameters. The parameters are substituted at each use site; there is no erasure and no runtime dispatch.

```
generic_params ::= '<' generic_param { ',' generic_param } '>'
generic_param  ::= IDENTIFIER [ ':' IDENTIFIER { '+' IDENTIFIER } ]

generic_args   ::= '<' type_arg { ',' type_arg } '>'
type_arg       ::= type | INT_LIT
```

### Declaring a generic

A generic function:

```lucid
const identity<T> fn (v T) -> T = {
    return v;
};
```

A generic struct:

```lucid
struct Box<T> {
    value T;
}
```

A generic trait:

```lucid
trait Container<T> {
    FIELD value T;
}
```

A generic host type:

```lucid
TYPE Map<K, V> = #host(LucidMap)
```

### Using a generic

A generic is instantiated with explicit type arguments:

```lucid
let b Box<int> = Box<int> { value = 42 };
let s Box<string> = Box<string> { value = "hello" };

let x int = identity<int>(5);
let y string = identity<string>("hi");

let m Map<string, int> = map_new<string, int>();
```

There is no type inference. Every type argument is written at the use site.

### Generic constraints

A parameter may be constrained to one or more traits:

```lucid
const magnitude<T : Vector2> fn (v T) -> float = {
    return sqrt(v.x * v.x + v.y * v.y);
};
```

Inside the function body, the fields and operations the trait requires are available on values of the parameter type. `v.x` and `v.y` work because `T : Vector2` and `Vector2` has `FIELD x float; FIELD y float;`.

Multiple constraints on one parameter are joined with `+`:

```lucid
const describeEntity<T : Vector2 + Named> fn (v T) -> string = {
    return v.name ++ " at (" ++ toStr(v.x) ++ ", " ++ toStr(v.y) ++ ")";
};
```

Multiple parameters are separated with `,`:

```lucid
const distanceBetween<T : Vector2, U : Vector2> fn (a T, b U) -> float = {
    let dx float = a.x - b.x;
    let dy float = a.y - b.y;
    return sqrt(dx * dx + dy * dy);
};
```

### Monomorphization

A generic function or struct is compiled once per concrete instantiation. `Box<int>` and `Box<string>` are distinct types; `identity<int>` and `identity<string>` are distinct functions. The compiler generates a specialization for each distinct instantiation used in the program.

There is no type erasure, no tagged-slot representation, and no runtime dispatch. `#sizeof(T)`, `#tostr(v)`, `Simd<T, N>`, and trait constraints all resolve to concrete types at each instantiation.

### Bare generic names

A bare generic name is a family, not a value:

```lucid
const identity<T> fn (v T) -> T = { return v; };

let f fn (int) -> int = identity;       -- error: 'identity' is a family
let g fn (int) -> int = identity<int>;  -- OK: a specialization
```

The error:

```
error: 'identity' names a generic function, not a value
   = help: supply type arguments to refer to a specialization:
     identity<int>
```

The rule applies everywhere a value is required: the right-hand side of an assignment, a call argument, an array element, and so on. A generic name is only legal as the callee of a call expression with explicit type arguments (`identity<int>(5)`) or as a bare specialization reference (`identity<int>`).

### Generic `TYPE` declarations

A generic `TYPE` declares a type family:

```lucid
TYPE Map<K, V> = #host(LucidMap)
TYPE Box<T> = struct { value T; }
```

The family is not a type itself. `Map` is not a type; `Map<string, int>` is.

### Constraints on `TYPE` parameters

A `TYPE` declaration's generic parameters may carry constraints:

```lucid
TYPE Cache<K : Eq, V : Stringable> = #host(LucidCache)
```

The constraints are checked at each instantiation. A use of `Cache<Foo, Bar>` is a compile error if `Foo` does not satisfy `Eq` or `Bar` does not satisfy `Stringable`.

### Generic traits and generic constraints

A generic trait is used as a constraint with a concrete instantiation:

```lucid
trait Container<T> {
    FIELD value T;
    FIELD count uint;
}

const first<T, C : Container<T>> fn (c C) -> T? = {
    if c.count == 0 { return nil; }
    return c.value;
};
```

The constraint `C : Container<T>` binds `C` to "some type satisfying `Container<T>`". The parameter `T` is also a parameter of the function, so `first<int, Box<int>>(someBox)` works, and `first<int, Stack<int>>(someStack)` works if `Stack` also satisfies `Container<int>`.

### Compiler-inserted `satisfy` blocks

The compiler inserts two `satisfy` blocks automatically:

- For every `TYPE X = struct { ... }` declaration, `satisfy StructType for X { }`.
- For every `TYPE X = enum { ... }` declaration, `satisfy EnumType for X { }`.

`StructType` and `EnumType` are compiler-provided traits with no clauses. They are used by the standard library's `toStr` fallback ([`toStr`](#tostr)). User code may use them as constraints:

```lucid
const describe<T : StructType> fn (v T) -> string = {
    return type_name<T>();
};
```

The compiler-inserted `satisfy` blocks are the only ones the compiler adds. Every other `satisfy` block must be written by a core script or by user code.

---

## Function Types

A function type describes the shape of a callable value. The `fn` and `cls` markers distinguish the two runtime representations.

```
func_type ::= stage { '->' stage } [ '->' type ]
stage     ::= ( 'fn' | 'cls' ) group
group     ::= '(' [ type_list ] ')'
type_list ::= type { ',' type } [ ',' '...' type ]
```

### The two markers

| Marker | Runtime value             | Words | Captures state?              | Is a resource? |
| ------ | ------------------------- | ----- | ---------------------------- | -------------- |
| `fn`   | bare function pointer     | 1     | No                           | No             |
| `cls`  | fat pointer `{func, env}` | 2     | Yes — refcounted environment | Yes            |

`fn` is a plain code address. It has no environment, so it cannot capture. A `fn` value is cheap to copy (one word) and is not refcounted.

`cls` is a fat pointer: a code address plus an environment pointer. The environment holds the closure's captured variables, and it is refcounted. A `cls` value's copy retains the environment; its drop releases.

### The markers are mandatory

Every stage in a function type carries its own marker:

```lucid
fn (int) -> int                         -- one stage, marker fn
cls (int) -> bool                       -- one stage, marker cls
fn (a int) -> cls (b int) -> int        -- two stages, first fn, then cls
fn (a int) fn (b int) -> int            -- two stages, both fn
```

The markers are per-stage. A missing marker is a parse error:

```
error: expected 'fn' or 'cls' before parameter group
   --> main.luc:1:14
    |
  1 | let f (int) -> int = ...;
    |       ^
```

The parser rejects the malformed form immediately and suggests the correct one.

### The four-cell shape table

When a declaration's initializer is an anonymous function literal, Sema infers the literal's shape and checks it against the declared type. The rule is per stage:

| Declared stage | Body infers | Result                                      |
| -------------- | ----------- | ------------------------------------------- |
| `fn`           | `fn`        | ✅ direct match                              |
| `fn`           | `cls`       | ❌ error — body captures, declared says bare |
| `cls`          | `fn`        | ✅ coerce `fn → cls` (wrap with null env)    |
| `cls`          | `cls`       | ✅ direct match                              |

The error in row 2 names the stage and the captured variable:

```
error: function declared 'fn' but its body captures 'n'
   --> main.luc:4:20
    |
  4 |     return (b int) -> int { return a + n; };
    |            ^^^^^^^^^^
    |
   = note: a stage marked 'fn' is a bare function pointer and cannot
     carry captured state
   = help: change 'fn' to 'cls' at the enclosing stage:
     fn (a int) -> cls (b int) -> int
```

When the initializer is any other expression (an identifier, a call), Sema does not infer a shape. It checks the initializer's type for assignability to the declared type, and applies the `fn → cls` coercion if applicable.

### The `fn → cls` coercion

A `fn` value can be used where a `cls` value is expected:

```lucid
const add fn (a int, b int) -> int = { return a + b; };

let c cls (int) -> int = add;    -- OK: fn coerced to cls
```

The compiler wraps the `fn` with a null environment. The wrapped value is a `cls` whose environment pointer is null; calling it does not touch the environment.

The reverse — using a `cls` where a `fn` is expected — is never allowed:

```lucid
let f fn (int) -> int = someClosure;    -- ERROR: cannot unwrap a cls
```

The `fn → cls` coercion is the only implicit conversion in the language.

### Anonymous function literals

A function literal's leading group carries no marker:

```lucid
let f = (x int) -> int { return x * 2; };
```

The literal's shape is inferred from whether its body captures anything. A literal with no captures is `fn`; a literal with captures is `cls`. Sema computes this and checks it against the declared type at the binding site.

Subsequent stages in a curried literal carry markers:

```lucid
let makeAdder = (n int) -> cls (int) -> int {
    return (b int) -> int { return n + b; };
};
```

The literal's leading group `(n int)` has no marker. The inner `cls (int) -> int` stage is a type position and carries its marker.

### Curried function types

A curried function type is a chain of stages:

```lucid
fn (a int) -> cls (b int) -> int
```

This is a two-stage function. The first stage takes `a int` and returns a `cls (b int) -> int`. The recursion terminates at a non-function return type.

The two spellings — adjacent stages (`fn (a int) fn (b int) -> int`) and arrow-separated stages (`fn (a int) -> fn (b int) -> int`) — parse to the same recursive `FuncTypeAST`. The parser desugars adjacency.

### Function-typed values in containers

A function type can appear anywhere a type can: as a struct field, an array element, a parameter, a return type:

```lucid
struct Handler {
    cb cls (int) -> unit;
}

let callbacks [*]fn (int) -> int = [double, triple];
```

The `fn`/`cls` markers are part of the type and are checked at every position.

### Shape checking in struct fields

A struct field declared with a function type follows the same shape rules:

```lucid
struct Sorter {
    compare fn (int, int) -> int;
}
```

The field's declared type says `fn`. Any assignment to it must be a `fn` (or coerced from a `fn`). Assigning a capturing closure to a `fn` field is a compile error:

```lucid
let s Sorter = Sorter {
    compare = (a int, b int) -> int { return a - b; },    -- OK: no captures
};

let threshold int = 5;
let s2 Sorter = Sorter {
    compare = (a int, b int) -> int { return a - b + threshold; },    -- ERROR: captures
};
```

The fix is to declare the field `cls` if it needs to accept capturing closures.

### Calling a function value

A function value is called with `()`:

```lucid
let f fn (int) -> int = double;
let y int = f(5);

let c cls (int) -> int = makeAdder(3);
let z int = c(7);
```

The call syntax is the same for both shapes. The compiler emits a direct call for `fn` values and an environment-loaded call for `cls` values; the user does not distinguish.

---

## Array and Sequence Types

Lucid has three array shapes:

```
array_type ::= '[' array_size ']' type
array_size ::= '*' | '_' | INT_LIT
```

- `[*]T` — dynamic array. Owned heap buffer; grows and shrinks.
- `[_]T` — slice. A borrowed view over a contiguous range of another array.
- `[N]T` — fixed array. Stack-allocated; length fixed at compile time.

### Dynamic arrays

```lucid
let xs [*]int = [1, 2, 3];
let ys [*]int = [];
```

A `[*]T` owns its buffer. Its copy deep-copies the buffer:

```lucid
let a [*]int = [1, 2, 3];
let b [*]int = a;    -- deep copy: b has its own buffer
b[0] = 99;           -- a[0] is still 1
```

Array operations are declared in the core script:

```lucid
FN array_len<T>    (a [_]T)            -> uint   = #builtin(array_len)
FN array_push<T>   (a &[*]T, v T)      = #builtin(array_push)
FN array_pop<T>    (a &[*]T)           -> T?     = #builtin(array_pop)
FN array_insert<T> (a &[*]T, i uint, v T) = #builtin(array_insert)
FN array_remove<T> (a &[*]T, i uint)   = #builtin(array_remove)
FN array_resize<T> (a &[*]T, n uint)   = #builtin(array_resize)
FN array_clear<T>  (a &[*]T)           = #builtin(array_clear)
```

### Slices

A slice is a borrowed view. It does not own its backing buffer; the buffer must outlive the slice.

```lucid
let xs [*]int = [10, 20, 30, 40, 50];
let sub [_]int = xs[1..3];    -- view over elements 1, 2, 3
```

A slice is created by indexing an array with a range:

```
slice_expr ::= expr '[' [ expr ] range_op [ expr ] ']'
range_op   ::= '..' | '..<'
```

- `..` is inclusive of the end bound.
- `..<` is exclusive.
- Omitting the start defaults to `0`.
- Omitting the end defaults to the array's length.

```lucid
xs[1..3]     -- elements at indices 1, 2, 3
xs[1..<3]    -- elements at indices 1, 2
xs[..<2]     -- elements at indices 0, 1
xs[3..]      -- elements from index 3 to the end
xs[..]       -- all elements
```

Slice bounds are runtime-checked. A start or end outside `[0, len]` is a runtime panic unless guarded with `??`:

```lucid
let bad [_]int = xs[1..99];          -- PANIC: end out of bounds
let ok  [_]int = xs[1..99] ?? [];    -- fallback to empty slice
```

Writing through a slice modifies the backing array:

```lucid
let buf [*]int = [0, 0, 0];
let window [_]int = buf;
window[0] = 42;    -- buf[0] is now 42
```

A slice cannot outlive its backing buffer. The compiler checks this statically; a slice cannot be returned from a function, stored in a struct field, or captured by a closure.

### Fixed arrays

A fixed array has a compile-time length:

```lucid
let rgb   [3]uint8  = [255, 128, 0];
let mat4  [16]float = [1.0, 0.0, ..., 1.0];
```

Fixed arrays are stack-allocated. Their copy is a full element copy:

```lucid
let a [3]int = [1, 2, 3];
let b [3]int = a;    -- deep copy
```

A fixed array can be passed where a slice is expected:

```lucid
const sum fn (xs [_]int) -> int = { ... };
let arr [5]int = [1, 2, 3, 4, 5];
sum(arr);    -- OK: fixed array coerces to slice
```

The coercion is implicit and free: the fixed array's storage is already contiguous, and the slice header is constructed with the array's length.

### Element nullability

`?` and `!` bind to the element type, not the array type:

```lucid
[*]int?    -- array of nullable int
[*]int!    -- array of fallible int
```

A nullable array does not exist. An "empty array" is the way to signal "no array." The empty array is written `[]` and is a valid value of any array type.

### Indexing

An array is indexed with `[]`:

```lucid
let x int = xs[0];
xs[i] = 42;
```

The index type is `uint` for arrays and slices. A negative index is a compile error. An out-of-bounds index is a runtime panic unless guarded with `??`:

```lucid
let x int = xs[i];           -- PANIC if i >= len
let y int = xs[i] ?? 0;      -- fallback to 0
```

A fixed array indexed with a literal in bounds is checked at compile time:

```lucid
let a [5]int = ...;
let x int = a[0];    -- OK: literal index, in bounds
let y int = a[10];   -- ERROR: literal index out of bounds
```

A fixed array indexed with a runtime value is checked at runtime, like any other array.

### Iterating

A `for` loop iterates an array with an index and a value:

```lucid
for i uint, x int in xs { ... }
```

The index is `uint`. Discarding either binding is done with `_`:

```lucid
for _, x int in xs { ... }    -- values only
for i uint, _ in xs { ... }   -- indices only
```

The first binding is required by the grammar even when discarded. See [Enums](#enums) for the full `for` grammar.

### Array literals

An array literal is a bracketed list:

```lucid
let xs [*]int = [1, 2, 3];
let ys [*]int = [];
let mat [3][3]float = [
    [1.0, 0.0, 0.0],
    [0.0, 1.0, 0.0],
    [0.0, 0.0, 1.0],
];
```

The literal's type is inferred from its declared type. An empty literal `[]` requires a declared type; `[]` alone is a compile error.

The elements must all be of the same type. A mixed literal is a compile error.

---

## References and `Weak<T>`

### References

A reference is written `&T`:

```
ref_type ::= '&' type
```

A `&T` is a strong reference to a value of type `T`. The referent's storage is refcounted; copying the reference increments the count, dropping it decrements. When the last reference drops, the referent is freed.

```lucid
struct Player { name string; health int; }

let p Player = Player { name = "alice", health = 100 };
let r &Player = p;       -- r aliases p
r.health = 50;           -- p.health is now 50
```

References may be stored in struct fields, array elements, returned from functions, and captured by closures. There is no Downward Flow Rule.

```lucid
struct Node {
    value int;
    next  &Node?;
}

let a Node = Node { value = 1, next = nil };
let b Node = Node { value = 2, next = a };
```

### Value vs reference

The distinction between `T` and `&T`:

| Type | Semantics | Storage             | Refcounted                            |
| ---- | --------- | ------------------- | ------------------------------------- |
| `T`  | value     | inline              | no (unless `T` is a heap-backed type) |
| `&T` | reference | pointer to referent | yes — the referent is refcounted      |

A value of type `T` copies on assignment. A value of type `&T` aliases: both bindings point at the same storage.

Heap-backed types (`[*]T`, `string`, closures, `Map<K,V>`) have refcounted backing stores. Their copy shares the backing store and increments its refcount. The `T` type itself is a small header; the backing store is where the data lives.

### Weak references

A `Weak<T>` is a reference that does not increment the referent's refcount:

```
weak_type ::= 'Weak' '<' type '>'
```

```lucid
let p Player = Player { name = "alice", health = 100 };
let w Weak<Player> = weak(p);        -- does not keep p alive

-- later, after p might have been freed:
if let r &Player = upgrade(w) {
    println(r.name);
} else {
    println("player is gone");
}
```

The three operations:

```lucid
FN weak<T>    (v &T)      -> Weak<T> = #builtin(weak)
FN upgrade<T> (w Weak<T>) -> &T?     = #builtin(upgrade)
```

`weak(v)` constructs a weak reference from a strong one. `upgrade(w)` returns a strong reference if the referent is still alive, or `nil` if it has been freed.

### Cycles

A cycle of strong references keeps every value in the cycle alive:

```lucid
struct Node {
    value int;
    next  &Node?;
    prev  &Node?;
}

let a Node = Node { value = 1, next = nil, prev = nil };
let b Node = Node { value = 2, next = nil, prev = nil };
a.next = b;
b.prev = a;    -- cycle: a → b → a
```

Neither `a` nor `b` ever drops to refcount zero, because each holds a strong reference to the other. The cycle leaks.

The fix is to make one edge weak:

```lucid
struct Node {
    value int;
    next  &Node?;
    prev  Weak<Node>?;
}

a.next = b;
b.prev = weak(a);    -- does not keep a alive
```

When the last external reference to `a` drops, `a` is freed, and `b.prev` becomes a dangling weak reference (`upgrade` returns `nil`).

The general rule: **the back edge in an ownership graph is weak.** Forward edges (ownership, containment) are strong; back edges (parent pointers, listener references) are weak.

The compiler warns on the obvious same-scope cycles it can detect:

```
warning: 'a.next' and 'b.prev' form a reference cycle
   --> main.luc:8:5
    |
  8 |     b.prev = a;
    |     ^^^^^^^^^^^
    |
   = help: consider making one edge 'Weak<T>':
     prev Weak<Node>?;
```

The warning is best-effort. Many cycles cannot be detected statically; the user is responsible for the rest.

### Cyclic structures and self-reference

A struct field whose type recursively contains the struct must be nullable (`?`). The `?` lowers the field to a pointer, which breaks the recursion. This applies to references as well:

```lucid
struct Node {
    value int;
    next  &Node?;    -- OK: reference is a pointer, nullable
}
```

A non-nullable reference to the struct is still infinite size, because the reference's slot in the struct is one pointer, but the referent is the struct... wait. Actually a `&Node` is one pointer regardless of the referent's size. So `next &Node` (non-nullable) is legal — the field is a pointer, and the referent is elsewhere.

Hmm, let me reconsider. The self-reference rule was about the *inline* case. `next Node` (non-nullable, no reference) means the field's storage is a `Node` inside the struct, which is infinite. `next &Node` means the field's storage is a pointer to a `Node`, which is one word regardless of the referent's size. So `next &Node` is legal.

But wait — for a value field `next Node?`, the `?` is what makes the storage a pointer rather than inline. For a reference field `next &Node`, the `&` already makes it a pointer. So `next &Node` should be legal, and `next &Node?` should also be legal (nullable pointer).

Let me correct the earlier rule: the self-reference rule applies to *value* fields, not reference fields. A `&Node` field is a pointer; it is never infinite size. The rule should be stated for `T` fields, and reference fields are always fine.

I'll update [Recursive fields](#recursive-fields) to say this. For now, note that this section's example uses `next &Node?` and that's legal.

---

## Nullable, Fallible, and Combined Types

### The three suffixes

```
nullable_type ::= type '?'
fallible_type ::= type '!'
combined_type ::= type '?!'
```

- `T?` — a value that may be `T` or `nil`.
- `T!` — a value that may be `T` or `err`.
- `T?!` — a value that may be `T`, `nil`, or `err`.

The order is fixed: `?!` is the only valid combination. `!?` is a parse error.

The suffixes bind to the immediately preceding type. In a curried function type, they attach to the innermost return type, not to the whole chain.

### The lowering

`T?` lowers to `Option<T>`:

```lucid
struct Option<T> {
    has   bool;
    value T;
}
```

`T!` lowers to `Fallible<T>`:

```lucid
struct Fallible<T> {
    ok    bool;
    value T;
}
```

`T?!` lowers to `Both<T>`:

```lucid
struct Both<T> {
    tag   int;    -- 0 = nil, 1 = value, 2 = err
    value T;
}
```

These are declared in the core script. The user writes `T?`, `T!`, `T?!`; the compiler wraps and unwraps the structs automatically.

### `nil` and `err`

`nil` and `err` are boot literals. Their type is inferred from context:

```lucid
let x int? = nil;         -- nil of type int?
let y string! = err;      -- err of type string!
let z Vec2?! = nil;       -- nil of type Vec2?!
let w Vec2?! = err;       -- err of type Vec2?!
```

A `nil` assigned to a non-nullable type is a compile error. An `err` assigned to a non-fallible type is a compile error.

### Narrowing

The compiler tracks the state of nullable and fallible values through control flow. A value that has been checked is narrowed to its non-sentinel type for the rest of the scope in which the check holds.

The narrowing conditions are:

```lucid
x == nil    -- inside the block: x is nil; after an exit: x is non-nullable
x != nil    -- inside the block: x is non-nullable
x == err    -- inside the block: x is err; after an exit: x is non-fallible
x != err    -- inside the block: x is non-fallible
```

For `T?!`:

```lucid
x == nil or x == err    -- after an exit: x is T
```

Narrowing is applied inside the `then` block of an `if`, and inverse narrowing is applied after a standalone `if` (no `else`) whose body exits.

```lucid
if x == nil { return; }
-- x is non-nullable here

if x != nil {
    -- x is non-nullable here
}
-- x is nullable again here

if x == nil or x == err { return; }
-- x is T here
```

### Forbidden operations on an un-narrowed value

An un-narrowed `T?`, `T!`, or `T?!` value cannot be used as a `T`:

```lucid
let x int? = nil;

let y int = x + 1;       -- ERROR: cannot use int? in arithmetic
println(toStr(x));       -- ERROR: cannot pass int? to a parameter expecting int
```

The only operations allowed on an un-narrowed value are the narrowing comparisons themselves, `??`, and passing it to a function that accepts `T?`/`T!`/`T?!`.

### The `??` fallback

`x ?? y` evaluates to `x` if `x` is a value, and to `y` if `x` is `nil`, `err`, or both:

```lucid
let a int? = nil;
let b int = a ?? 0;      -- b is 0

let c int! = err;
let d int = c ?? -1;     -- d is -1

let e Vec2?! = nil;
let f Vec2 = e ?? Vec2 { x = 0.0, y = 0.0 };
```

The result type is the plain type `T`, not a nullable or fallible type. `??` always resolves the sentinel.

### Runtime panics

Some primitive operations can fail at runtime in ways the type system does not track:

- Integer division by zero.
- Array indexing out of bounds.

A failing operation **panics**: the program terminates with a diagnostic. `??` at the operation's site converts the panic into a fallback:

```lucid
let a int = 10 / d;         -- PANIC if d == 0
let b int = 10 / d ?? -1;   -- fallback to -1 if d == 0
```

A panic is not catchable. The only way to prevent it is `??` at the risky expression. This is the same stance the design takes everywhere else: recovery must be written at the point of risk, not somewhere else up the call stack.

### `if let` binding

A nullable value can be bound in an `if` condition with `if let`:

```lucid
if let x int = someNullable {
    -- x is int here
    println(toStr(x));
}
```

The binding introduces a non-nullable value in the `then` block. The check is `!= nil` under the hood. The `if let` form works for `T?`, `T!`, and `T?!`; for the fallible cases, the check is `!= err` (or `!= nil and != err` for `T?!`).

`if let` also composes with `while`:

```lucid
let d Deferred<int> = start fetch();
while let x int = upgrade(w) {
    process(x);
}
```

### The `Stringable` trait

`toStr` ([`toStr`](#tostr)) is the stringification mechanism — every call to `toStr(v)` resolves through the `CALL 'toStr'` table. `Stringable` is not a second mechanism; it is a trait whose sole `REQUIRE` clause asks for that same operation, so it can be used as a constraint:

```lucid
trait Stringable {
    REQUIRE CALL 'toStr' (self Self) -> string;
}
```

A type satisfies `Stringable` by declaring a `DEF CALL 'toStr'` for itself, most conveniently inside a `satisfy` block, which provides the `DEF` and asserts the conformance in one place:

```lucid
satisfy Stringable for Vec2 {
    DEF CALL 'toStr' (v Vec2) -> string = {
        return "(" ++ toStr(v.x) ++ ", " ++ toStr(v.y) ++ ")";
    };
}
```

The `DEF` declared inside the `satisfy` block is an ordinary, concrete `toStr` for `Vec2` — the same kind of `DEF` a top-level declaration would produce — and it takes precedence over the compiler-generated struct fallback ([Struct and enum `toStr`](#struct-and-enum-tostr)) by ordinary overload resolution. Writing the same `DEF` at top level, outside a `satisfy` block, gives `Vec2` the custom `toStr` without formal `Stringable` conformance; see [The `toStr` fallback chain](#the-tostr-fallback-chain) for how that distinction affects resolution.

---

## Statements

A statement is a unit of execution inside a block. Statements are the only thing a block contains, and a block is the only place statements appear.

```
block       ::= '{' { statement } '}'

statement   ::= expr_stmt ';'
              | assign_stmt ';'
              | value_decl
              | return_stmt ';'
              | break_stmt ';'
              | continue_stmt ';'
              | if_stmt
              | while_stmt
              | do_while_stmt ';'
              | for_stmt
              | switch_stmt
              | spawn_stmt ';'
              | start_stmt ';'
              | await_stmt ';'
              | local_type_decl

expr_stmt   ::= expr
assign_stmt ::= expr '=' expr
return_stmt ::= 'return' [ expr ]
break_stmt  ::= 'break'
continue_stmt ::= 'continue'

local_type_decl ::= type_decl | trait_decl | satisfy_decl | def_decl
```

### The four categories

**Declarations.** `value_decl` (`const`, `let`, `FN`), `local_type_decl` (`TYPE`, `trait`, `satisfy`, `DEF`). A declaration introduces a name into the current block's scope.

**Bindings and assignments.** `assign_stmt` reassigns an existing `let` binding. `expr_stmt` evaluates an expression for its side effect and discards the result.

**Control flow.** `if`, `while`, `do`/`while`, `for`, `switch`. These are described in [`if` / `else`](#if-else)–[Pattern Matching (Future)](#pattern-matching-future).

**Jumps.** `return`, `break`, `continue`. These exit the current function, loop, or iteration.

**Concurrency.** `spawn`, `start`, `await`. Described in [Expression Forms](#expression-forms).

### Semicolons

A statement that ends with a `block` takes no trailing `;`:

- `if_stmt`, `while_stmt`, `for_stmt`, `switch_stmt` all end with `}` and take no `;`.

A statement that ends with an expression takes a `;`:

- `do_while_stmt` ends with the condition expression and requires `;`.
- `expr_stmt`, `assign_stmt`, `return_stmt`, `break_stmt`, `continue_stmt`, `spawn_stmt`, `start_stmt`, `await_stmt` all require `;`.

A declaration that ends with a `block` value still requires `;`:

- `const f fn (int) -> int = { return 0; };` — the `}` closes the block value, not the statement; the `;` terminates the declaration.

The rule: **the exemption is for statements whose outermost construct is one of the block-terminated forms**, not for any statement that happens to end with `}`.

### Declarations inside blocks

`const`, `let`, `FN`, `TYPE`, `trait`, `satisfy`, and `DEF` may all appear inside a block:

```lucid
const compute fn () -> int = {
    struct Local { x int; y int; }
    const add fn (a int, b int) -> int = { return a + b; };
    let p Local = Local { x = 1, y = 2 };
    return add(p.x, p.y);
};
```

A local declaration is visible from its declaration point to the end of the enclosing block. A local type is visible to nested closures declared after it in the same block:

```lucid
const f fn () -> fn () -> Direction {
    enum Direction { North = 0; East = 1; }
    return () -> Direction {
        return Direction.North;
    };
};
```

A local type may not appear in the header of the function that declares it:

```lucid
const f fn () -> Direction {    -- ERROR: Direction not yet in scope
    enum Direction { North = 0; East = 1; }
    return Direction.North;
};
```

The header is resolved before the body's declarations come into scope. A closure returned from later in the body is fine.

### Attributes on local declarations

Attributes (`@[inline]`, `@[deprecated]`, etc.) are allowed on local declarations. `@[export]` is **not** allowed inside a block; it is top-level only. The parser rejects `@[export]` at a local position.

### Scope and shadowing

Each block introduces a new scope. A declaration in an inner block shadows a declaration of the same name in an outer block:

```lucid
let x int = 1;
{
    let x int = 2;    -- shadows outer x
    println(toStr(x));    -- prints 2
}
println(toStr(x));    -- prints 1
```

A declaration in the same scope with the same name is a redeclaration error:

```lucid
let x int = 1;
let x int = 2;    -- ERROR: 'x' already declared in this scope
```

### Return

A `return` statement exits the enclosing function:

```lucid
const f fn (x int) -> int = {
    if x < 0 { return 0; }
    return x * 2;
};
```

The returned value's type must match the function's declared return type. A `return` with no value is legal only in a function returning `unit`.

A function that returns a value must return on every path:

```lucid
const f fn (x int) -> int = {
    if x < 0 { return 0; }
    -- ERROR: not all paths return a value
};
```

### Break and continue

`break` exits the nearest enclosing loop. `continue` skips to the next iteration of the nearest enclosing loop. Both are errors outside a loop body.

```lucid
for i uint, x int in xs {
    if x < 0 { continue; }
    if x > 100 { break; }
    process(x);
}
```

### Expression statements

An expression used as a statement evaluates for its side effect:

```lucid
println("hello");
doSomething();
```

The result is discarded. A function returning a non-`unit` value can be called as a statement; its return value is ignored. The compiler may warn if the value looks like it should have been used (e.g., a fallible call whose `err` state is never checked).

### Assignment

An assignment stores a value into an existing `let` binding:

```lucid
let x int = 0;
x = 5;
x = x + 1;
```

The left-hand side must be an l-value: a `let` binding, a mutable field access, or a mutable index. Assigning to a `const` binding, a `const` field, or a non-l-value is a compile error.

Compound operators (`+=`, `-=`, `*=`, `/=`, `%=`, `**=`, `&=`, `|=`, `^=`, `<<=`, `>>=`) desugar to `x = x op rhs` and follow the same rules.

---

## `if` / `else`

The `if` statement tests a condition and executes a block:

```
if_stmt ::= 'if' expr block { 'else' 'if' expr block } [ 'else' block ]
```

### Basic form

```lucid
if x > 0 {
    println("positive");
} else if x < 0 {
    println("negative");
} else {
    println("zero");
}
```

The condition must be `bool`. There is no truthiness coercion; `if x` where `x` is an `int` is a compile error.

### Narrowing

The compiler applies type narrowing inside the `then` block based on the condition:

```lucid
if x != nil {
    -- x is int here (not int?)
    println(toStr(x + 1));
}
-- x is int? here
```

The narrowing rules are in [Narrowing](#narrowing). `if let` provides a binding form:

```lucid
if let v int = x {
    -- v is int here
}
```

### Inverse narrowing

After a standalone `if` (no `else`) whose body exits, the inverse of the condition holds:

```lucid
if x == nil { return; }
-- x is non-nullable here
```

The inverse-narrowing rules are in [Narrowing](#narrowing). They apply to `return`, `break`, and `continue` exits. They do not apply after an `if` with an `else` branch, or after an `else if` chain.

### `if` expression

`if` has an expression form that produces a value:

```
if_expr ::= 'if' expr '??' expr 'else' expr
```

```lucid
let grade string = if score >= 60 ?? "pass" else "fail";
let label string = if n < 0 ?? "negative" else if n == 0 ?? "zero" else "positive";
```

In expression form, `else` is required and both branches must produce compatible types. The `??` separates the condition from the then-branch. The expression form nests right-associatively.

The expression form is distinct from the statement form: the statement form has an optional `else` and produces no value; the expression form has a required `else` and produces a value. The parser distinguishes them by the `??` after the condition.

---

## Loops

Lucid has three loop forms: `while`, `do`/`while`, and `for`.

### `while`

```
while_stmt ::= 'while' expr block
```

The condition is tested before each iteration:

```lucid
let i int = 0;
while i < 10 {
    println(toStr(i));
    i = i + 1;
}
```

The condition must be `bool`. Narrowing applies inside the body, and inverse narrowing applies after the loop if the loop exits via `break`.

### `do` / `while`

```
do_while_stmt ::= 'do' block 'while' expr ';'
```

The body runs at least once, and the condition is tested after each iteration:

```lucid
let i int = 0;
do {
    println(toStr(i));
    i = i + 1;
} while i < 10;
```

The `do`/`while` statement requires a trailing `;` because it ends with the condition expression, not a block.

### `for` — range iteration

```
range_for ::= 'for' IDENTIFIER type 'in' range_expr block
range_expr ::= expr range_op expr [ '..' expr ]
range_op  ::= '..' | '..<'
```

A range loop iterates a sequence of numbers:

```lucid
for i int in 0..10 {        -- 0, 1, 2, ..., 10
    println(toStr(i));
}

for i int in 0..<10 {       -- 0, 1, 2, ..., 9
    println(toStr(i));
}

for i int in 0..10..2 {     -- 0, 2, 4, 6, 8, 10
    println(toStr(i));
}

for i int in 10..0..-1 {    -- 10, 9, 8, ..., 0
    println(toStr(i));
}
```

The loop variable's type must match the range's element type. The optional step (after the second `..`) advances the value; a step of zero is a compile error. A negative step counts down.

The range bounds may be any expression of the same numeric type:

```lucid
for i int in start..end {
    ...
}
```

**Step is range-only.** Collections iterate step 1. Stepping an array is done by iterating a range over its indices.

### `for` — collection iteration

```
collection_for ::= 'for' IDENTIFIER type ',' IDENTIFIER type 'in' expr block
```

A collection loop iterates an array or a map with two bindings:

```lucid
for i uint, x int in xs {
    println(toStr(i) ++ ": " ++ toStr(x));
}

for k string, v int in scores {
    println(k ++ ": " ++ toStr(v));
}
```

The first binding's type depends on the collection:

| Collection             | First binding  | Second binding |
| ---------------------- | -------------- | -------------- |
| `[*]T`, `[_]T`, `[N]T` | `uint` (index) | `T` (element)  |
| `Map<K, V>`            | `K` (key)      | `V` (value)    |

Discarding either binding is done with `_`:

```lucid
for _, x int in xs { ... }       -- values only
for i uint, _ in xs { ... }      -- indices only
for k string, _ in scores { ... }  -- keys only
for _, v int in scores { ... }   -- values only
```

The two-binding form is required. A single binding on a collection is a compile error:

```lucid
for x int in xs { ... }    -- ERROR: collection iteration requires
                           -- both an index and a value
```

Use `for _, x int in xs` to iterate values only.

### Read-only loop bindings

The loop bindings are `const` within the body:

```lucid
for i uint, x int in xs {
    i = 0;    -- ERROR: cannot assign to loop binding 'i'
    x = 0;    -- ERROR: cannot assign to loop binding 'x'
}
```

A shadowing binding provides a mutable copy:

```lucid
for i uint, x int in xs {
    let idx uint = i;
    idx = idx + 1;    -- OK: shadows the loop binding
}
```

The read-only rule prevents the loop variable from being changed mid-iteration, which would make the loop's behavior undefined.

### Nested loops

Loops nest normally. `break` and `continue` apply to the nearest enclosing loop:

```lucid
for i uint, x int in xs {
    for j uint, y int in ys {
        if x == y { break; }      -- exits the inner loop
    }
    if x == 0 { continue; }       -- continues the outer loop
}
```

### Loop labels

Lucid does not have labeled loops. To break out of an outer loop, use a flag:

```lucid
let found bool = false;
for i uint, x int in xs {
    if found { break; }
    for j uint, y int in ys {
        if x == y {
            found = true;
            break;
        }
    }
}
```

Labeled loops are a possible future addition, but they are not in this version.

---

## `switch` and Pattern Matching

The `switch` statement dispatches on a single value and runs the matching case's block.

```
switch_stmt    ::= 'switch' expr '{' { case_clause } [ default_clause ] '}'
case_clause    ::= 'case' case_value { ',' case_value } ':' block
default_clause ::= 'default' ':' block

case_value     ::= literal
                 | IDENTIFIER '.' IDENTIFIER
                 | IDENTIFIER '.' IDENTIFIER '(' IDENTIFIER ')'
                 | literal range_op literal
```

### Basic form

```lucid
switch statusCode {
    case 200: { handleOk(); }
    case 404: { handleNotFound(); }
    case 500: { handleServerError(); }
    default:  { handleUnknown(); }
}
```

The subject's type must be one of: an integer type, `bool`, `char`, `string`, or an enum. Structs, arrays, floats, and function types are not valid `switch` subjects.

Cases are matched top to bottom. The first matching case runs. There is no fallthrough; each case is independent.

### Multiple values per case

A case may list several values:

```lucid
switch dir {
    case Direction.North, Direction.South: { moveVertical(); }
    case Direction.East,  Direction.West:  { moveHorizontal(); }
}
```

### Literal ranges

A case may match an inclusive or exclusive range of literal values:

```lucid
switch score {
    case 90..100:  { grade = "A"; }
    case 80..<90:  { grade = "B"; }
    case 70..<80:  { grade = "C"; }
    default:       { grade = "F"; }
}
```

Range bounds must be literals. A range with variable bounds is a compile error:

```lucid
case lo..hi: { ... }    -- ERROR: case range bounds must be literals
```

Ranges combine with comma-separated values:

```lucid
switch n {
    case 0, 1..9:  { println("single digit or zero"); }
    case 10..<100: { println("two digits"); }
    default:       { println("large"); }
}
```

### Enum exhaustiveness

A `switch` on an enum type without a `default` must cover every variant:

```lucid
switch dir {
    case Direction.North: { moveUp(); }
    case Direction.South: { moveDown(); }
    case Direction.East:  { moveRight(); }
    case Direction.West:  { moveLeft(); }
}
```

A missing variant is a compile error:

```
error: switch on 'Direction' does not cover variant 'West'
   = help: add a case for 'Direction.West', or add a 'default' clause
```

Adding a `default` clause suppresses the exhaustiveness check.

### Payload enums

A `switch` on a payload-carrying enum binds the payload in the case:

```lucid
switch v {
    case JsonValue.Num(n): { println("number: " ++ toStr(n)); }
    case JsonValue.Str(s): { println("string: " ++ s); }
    case JsonValue.Arr(a): { println("array of " ++ toStr(array_len(a))); }
    case JsonValue.Obj(o): { println("object with " ++ toStr(array_len(o)) ++ " keys"); }
}
```

The binding `(n)`, `(s)`, `(a)`, `(o)` introduces the payload in the case body. The payload's type is the variant's declared type. A payload variant without a binding is a compile error.

An integer variant is matched without a binding:

```lucid
switch token {
    case Token.Eof: { ... }
    case Token.Ident(s): { ... }
    case Token.Number(f): { ... }
}
```

`Token.Eof` and `Token.Ident(s)` coexist in the same `switch`. The parser distinguishes them by the presence of the `(binding)` after the variant name.

### Rejected case values

`case_value` is deliberately narrow. It is a literal, an enum variant, a payload-binding pattern, or a literal range. It is never a general expression:

```lucid
switch x {
    case a < b: { ... }        -- ERROR: case value cannot be a comparison
}

switch x {
    case err ?? something: { ... }    -- ERROR: case value cannot be a fallback
}

switch n {
    case lo..hi: { ... }        -- ERROR: range bounds must be literals
}
```

Conditions belong in `if` / `else if`, not in `switch`. A `switch` dispatches on a matched value, not on a computed condition.

### `switch` on nullable and fallible values

A `switch` on a `T?`, `T!`, or `T?!` value narrows the subject's state:

```lucid
switch x {
    case nil: { println("absent"); }
    case err: { println("failed"); }
    default:  { println("value: " ++ toStr(x)); }    -- x narrowed to T
}
```

The `nil` and `err` cases are recognized when the subject is nullable or fallible. The `default` clause handles the present-value case, and `x` is narrowed to `T` inside it.

This is `switch` on the sentinel states, not on the value itself. For a `T?` whose `T` is an enum, `switch x` matches `nil` and the enum's variants, but not an explicit `err` case (since `T?` has no `err`).

### No fallthrough

Each case is independent. There is no `fallthrough` keyword and no implicit fallthrough:

```lucid
switch n {
    case 1: { println("one"); }
    case 2: { println("two"); }
}
```

Only one of the two blocks runs. Running `switch 1` prints `"one"` and does not continue into `case 2`.

To share behavior between cases, list them in one case or call a shared function:

```lucid
switch n {
    case 1, 2, 3: { println("small"); }
    case 4, 5, 6: { println("medium"); }
}
```

### Jump table dispatch

For integer and enum subjects, the compiler emits a jump table when the case values are dense enough. This gives O(1) dispatch regardless of the number of cases. For sparse case values, the compiler falls back to a sequence of comparisons.

The choice is an optimization; the observable behavior is the same.

---

## Pattern Matching (Future)

Lucid does not currently have a general pattern-matching construct beyond `switch` and `if let`. The `switch` statement matches values, enum variants, payload bindings, and literal ranges. The `if let` binding narrows a nullable or fallible value.

A more general pattern-matching form — binding multiple values, destructuring tuples, and matching against structural patterns — is not part of this version. If a need arises, it would be a new statement form, not an extension of `switch`.

The current forms cover the common cases:

- Value dispatch: `switch`.
- Sentinel dispatch: `switch` on `T?`/`T!`/`T?!`, or `if let`.
- Enum variant dispatch: `switch` with payload bindings.
- Range dispatch: `switch` with literal ranges.

For more complex matches, the user composes `if`/`else if` with explicit comparisons and bindings.

---

## Expression Forms

An expression produces a value. Expressions appear inside statements, in declaration initializers, in conditions, and in arguments. The full grammar:

```
expr        ::= literal
              | identifier_expr
              | array_literal
              | struct_literal
              | call_expr
              | index_expr
              | slice_expr
              | field_expr
              | module_expr
              | unary_expr
              | binary_expr
              | assign_expr
              | null_coalesce_expr
              | pipeline_expr
              | func_literal
              | if_expr
              | range_expr
              | paren_expr

literal     ::= INT_LIT | FLOAT_LIT | STRING_LIT | CHAR_LIT
              | BOOL_LIT | 'nil' | 'err'

identifier_expr ::= IDENTIFIER [ '<' type_arg { ',' type_arg } '>' ]

paren_expr  ::= '(' expr ')'
```

The individual forms are described in the sections below. Precedence and associativity are in [Precedence and Associativity](#precedence-and-associativity).

---

## Literals

A literal is a value written directly in source.

```lucid
42                  -- INT_LIT
3.14                -- FLOAT_LIT
"hello"             -- STRING_LIT
"""raw text"""      -- raw STRING_LIT
'a'                 -- CHAR_LIT
true                -- BOOL_LIT
false
nil                 -- nil literal
err                 -- err literal
```

### Integer literals

Integer literals are written in decimal, hex (`0x`), binary (`0b`), or octal (`0o`):

```lucid
42
0xFF
0b1010
0o777
```

An integer literal is untyped at the lexer level. Its concrete type is determined by the surrounding declared type. When there is no target type, an integer literal defaults to `int`.

A literal that does not fit its target type is a compile error:

```lucid
let x uint8 = 300;    -- ERROR: 300 does not fit in uint8
```

### Float literals

Float literals require a decimal point:

```lucid
3.14
1.0
2.5e10
1.0E-9
```

A float literal's concrete type is determined by context. Default is `double`.

### String literals

Two forms:

**Normal string.** `"..."` — escapes are processed, no literal newlines, and `\(expr)` interpolates:

```lucid
"hello"
"line 1\nline 2"
"count: \(toStr(n))"
```

**Raw string.** `"""..."""` — escapes are not processed, no interpolation, literal newlines allowed, and the content may span multiple lines:

```lucid
"""
SELECT id, name
FROM users
"""
```

A single or double quote may appear freely inside a raw string. Only three consecutive quotes close it.

### Character literals

```lucid
'a'
'\n'
'\''
```

### Boolean literals

`true` and `false`.

### `nil` and `err`

`nil` and `err` are the sentinel literals. Their type is determined by context:

```lucid
let x int? = nil;
let y string! = err;
let z Vec2?! = nil;
```

`nil` is valid only where a nullable type is expected. `err` is valid only where a fallible type is expected.

### String interpolation

Inside a normal string, `\(expr)` evaluates `expr` and inserts its string form:

```lucid
let name string = "alice";
let age  int    = 30;
let greeting string = "hello \(name), you are \(toStr(age)) years old";
```

The expression must produce a `string`. A non-string expression is a compile error:

```lucid
let n int = 42;
let s string = "value: \(n)";    -- ERROR: n is int, not string
let s2 string = "value: \(toStr(n))";    -- OK
```

Interpolation is not available in raw strings.

---

## Identifier Expressions

A bare identifier names a value:

```lucid
x                -- a local or parameter
add              -- a function
Direction        -- an enum type name (in `.North`)
```

A generic function name with explicit type arguments is a specialization reference:

```lucid
identity<int>    -- a value of type (int) -> int
```

The `genericArgs` (the `<int>`) distinguish a specialization reference from a bare name. A bare generic name is a family, not a value; using it where a value is required is a compile error.

### Resolution

An identifier is resolved against the current scope chain: local bindings, parameters, enclosing function's bindings, module-level bindings, imported module's exported bindings. The first match wins.

If the name resolves to a struct field and `self` is in scope (inside a block-body field default), the identifier is lowered to a field access through `self`.

### Generic specialization references

A specialization reference is a value:

```lucid
const identity<T> fn (v T) -> T = { return v; };

let f fn (int) -> int = identity<int>;    -- OK: specialization reference
```

The reference is used where a function value of the specialization's type is expected. It is not a call; the value is the function itself, not its result.

### Module-qualified names

A name qualified by a module is a `module_expr`:

```lucid
math::sqrt
math::PI
```

See [Module Access](#module-access).

---

## Struct and Array Literals

### Struct literals

```
struct_literal ::= IDENTIFIER [ '<' type_arg { ',' type_arg } '>' ]
                   '{' { field_init } '}'
field_init     ::= IDENTIFIER '=' expr
```

```lucid
Point { x = 1.0, y = 2.0 }
Point { x = 1.0 }              -- y takes its default
Box<int> { value = 42 }
```

Field order in the literal is free. Fields with defaults may be omitted. Fields without defaults must be provided. A struct with only `@[opaque]` fields cannot be constructed with a literal.

The type name must resolve to a struct declaration. The field names must match the struct's declared fields. The field values' types must match the field's declared type.

### Array literals

```
array_literal ::= '[' [ expr { ',' expr } ] ']'
```

```lucid
[1, 2, 3]
[]
["hello", "world"]
[[1, 2], [3, 4]]
```

An array literal's element type is the type of its elements. All elements must have the same type. An empty literal `[]` requires a declared type:

```lucid
let xs [*]int = [];    -- OK
let ys = [];           -- ERROR: cannot infer element type
```

The declared type determines the array kind:

```lucid
let a [*]int = [1, 2, 3];       -- dynamic array
let b [3]int = [1, 2, 3];       -- fixed array
let c [_]int = [1, 2, 3];       -- ERROR: slice literal not allowed
```

A slice literal is not allowed; a slice is always created from another array.

---

## Field Access

```
field_expr ::= expr '.' IDENTIFIER
```

A field access reads or assigns a struct field:

```lucid
p.x
player.health
rect.origin.x
```

The left-hand side must be a struct value. The right-hand side names a field of that struct.

### L-value

A field access is an l-value if the base expression is an l-value and the field is not `const`:

```lucid
let p Point = Point { x = 1.0, y = 2.0 };
p.x = 5.0;                -- OK

const q Point = Point { x = 1.0, y = 2.0 };
q.x = 5.0;                -- ERROR: q is const
```

A `const` field is never an l-value, even when the containing value is `let`:

```lucid
struct Counter {
    const step int = 1;
    total int;
}

let c Counter = Counter { total = 0 };
c.total = 5;    -- OK
c.step  = 2;    -- ERROR: step is const
```

### Nested access

Field access chains left to right:

```lucid
a.b.c    -- equivalent to (a.b).c
```

### Enum variant access

`.Variant` on an enum type name reads a variant:

```lucid
Direction.North
JsonValue.Num(3.14)
```

A variant read is not an l-value.

---

## Indexing and Slicing

### Index expressions

```
index_expr ::= expr '[' expr ']'
```

An index expression reads or writes a single element:

```lucid
xs[0]
xs[i]
m["alice"]
```

The index type depends on the container:

- For arrays (`[*]T`, `[_]T`, `[N]T`), the index must be `uint`.
- For `Map<K, V>`, the index must be `K`.

An index expression is resolved through `DEF INDEX_GET` or `DEF INDEX_SET`. If no matching `DEF` exists for the container's type and index type, the expression is a compile error.

### Index l-value

An index expression is an l-value if the container is mutable and the index is legal:

```lucid
let xs [*]int = [1, 2, 3];
xs[0] = 99;                -- OK
xs[i] = 42;                -- OK if i < xs.length

let m Map<string, int> = map_new<string, int>();
m["alice"] = 10;           -- OK
```

The `INDEX_SET` `DEF` is invoked for assignment. For arrays, the operation is a store. For `Map`, the operation is an insert or replace.

### Bounds checking

An array index is bounds-checked at runtime:

```lucid
let x int = xs[i];         -- PANIC if i >= xs.length
let y int = xs[i] ?? 0;    -- fallback to 0 on out-of-bounds
```

A fixed array indexed with a literal in bounds is checked at compile time:

```lucid
let a [5]int = ...;
let x int = a[0];    -- OK
let y int = a[10];   -- ERROR: out of bounds
```

### Slice expressions

```
slice_expr ::= expr '[' [ expr ] range_op [ expr ] ']'
range_op   ::= '..' | '..<'
```

A slice expression creates a view over a contiguous range:

```lucid
xs[1..3]     -- inclusive: elements 1, 2, 3
xs[1..<3]    -- exclusive: elements 1, 2
xs[..<2]     -- elements 0, 1
xs[3..]      -- elements 3 to end
xs[..]       -- all elements
```

A slice's type is `[_]T`. The bounds are runtime-checked. A start or end out of range is a runtime panic unless guarded with `??`:

```lucid
let sub [_]int = xs[1..99] ?? [];
```

A slice cannot outlive its backing array. The compiler checks that a slice is not returned, not stored in a struct field, and not captured by a closure.

### Slice of a fixed array

A fixed array may be sliced:

```lucid
let arr [10]int = ...;
let sub [_]int = arr[2..5];
```

The slice views the fixed array's storage, which is stack-allocated. The slice is valid as long as the fixed array binding is in scope.

---

## Module Access

```
module_expr ::= IDENTIFIER '::' IDENTIFIER
```

A module access reads a name exported from another module:

```lucid
math::sqrt
math::PI
io::println
```

The left-hand side is the module's name (the file's path relative to the package root, with dots). The right-hand side is a name exported by that module.

### Calling an exported function

```lucid
math::sqrt(2.0)
io::println("hello")
```

A module-qualified call resolves the member against the module's export table and dispatches through the `CALL` table if the member is a `DEF CALL`.

### Accessing an exported value

```lucid
let pi float = math::PI;
```

### Module-qualified types

A type may be qualified by its module:

```lucid
let b parser::Box<int> = parser::Box<int> { value = 42 };
```

Module qualification of a type name is allowed when the type is exported from another module.

### `::` vs `.`

`::` is module access. `.` is struct field access. The two are distinct:

```lucid
math::sqrt        -- OK: sqrt is a module member
math.sqrt         -- ERROR: math is not a struct
player.health     -- OK: health is a struct field
player::health    -- ERROR: player is not a module
```

A `::` access never resolves to a struct field, and a `.` access never resolves to a module member.

### Static struct members

A static struct member is accessed with `::`:

```lucid
Vec2::zero()
Vec2::fromAngle(0.0)
```

The left-hand side of `::` is either a module name or a struct name. The parser distinguishes them by resolution: if the left-hand side resolves to a module, the access is a module access; if it resolves to a struct, the access is a static member access.

### Nested modules

A module name may contain dots for subdirectories:

```lucid
core::io::println
```

The path `core.io` names the module; the access chain `core::io::println` reads `println` from that module. The parser treats `core::io` as a single module name and `println` as the member.

---

## Calls

```
call_expr ::= expr '(' [ arg_list ] ')'
arg_list  ::= expr { ',' expr }
```

A call invokes a function value or a function declaration:

```lucid
add(1, 2)
math::sqrt(2.0)
f(x)
obj.callback(42)
```

### Resolution

The callee expression is resolved to one of:

- A named function (`add`, `math::sqrt`).
- A function-typed binding (`f`).
- A struct field of function type (`obj.callback`).
- A generic specialization (`identity<int>`).
- A `CALL`-op-name (`toStr`).

For a named function or specialization, the compiler emits a direct call. For a function-typed value, the compiler emits a call through the value's code pointer (loading the environment first if the value is `cls`).

For a `CALL` op-name, the compiler resolves the call through the `CALL` table, keyed on the argument types.

### Argument matching

The arguments must match the callee's declared parameters in number and type. There is no implicit conversion other than the `fn → cls` coercion.

```lucid
const add fn (a int, b int) -> int = { return a + b; };
add(1, 2);              -- OK
add(1);                 -- ERROR: missing argument
add(1, 2, 3);           -- ERROR: too many arguments
add(1.0, 2.0);          -- ERROR: float where int expected
```

### Curried calls

A curried function is called with multiple argument groups:

```lucid
const clamp fn (lo int) fn (hi int) fn (v int) -> int = { ... };
clamp(0)(100)(42);
```

Each call returns a function value that takes the next group. The final call returns the result.

A partially-applied value is a first-class function:

```lucid
const clamp0to100 fn (v int) -> int = clamp(0)(100);
clamp0to100(42);    -- 42
```

### Module-qualified calls

A module-qualified name is called with `()`:

```lucid
io::println("hello")
math::sqrt(2.0)
```

See [Module Access](#module-access) for module access resolution.

### Calls on function-typed values

A function-typed value is called with `()`:

```lucid
let f fn (int) -> int = double;
f(5);                      -- direct call

let c cls (int) -> int = makeAdder(3);
c(7);                      -- closure call

obj.callback(42);          -- call through a struct field
```

The call syntax is identical for all cases. The compiler chooses the right call protocol based on the callee's type.

### Chained calls

A call's result can be called if it is itself a function:

```lucid
makeAdder(3)(7)    -- makeAdder(3) returns a function, which is called with 7
```

Chained calls are the curried form, written without intermediate bindings.

---

## Operators

Binary, unary, and assignment operators are syntactic forms whose meaning is determined by `DEF` declarations.

### Binary operators

```
binary_expr ::= expr binary_op expr
binary_op   ::= '+' | '-' | '*' | '/' | '%' | '**'
              | '==' | '!=' | '<' | '<=' | '>' | '>='
              | 'and' | 'or'
              | '&' | '|' | '^' | '<<' | '>>'
```

A binary expression `a op b` resolves through the `BINARY_OP` table:

- The compiler looks up `DEF BINARY_OP 'op' (a_type, b_type) -> result_type`.
- The exact-match `DEF` is preferred over a generic `DEF`.
- If no `DEF` matches, the expression is a compile error.

The `and` and `or` operators are short-circuit. The right-hand side is evaluated only if the left-hand side does not determine the result.

### Unary operators

```
unary_expr ::= ('-' | 'not' | '~') expr
```

A unary expression `op x` resolves through the `UNARY_OP` table.

### Assignment

```
assign_expr ::= expr '=' expr
```

An assignment stores a value into an l-value. The left-hand side must be an l-value; the right-hand side's type must be assignable to the left-hand side's type.

Compound assignment operators desugar to `x = x op rhs`:

```lucid
x += 1     -- desugars to x = x + 1
x *= 2     -- desugars to x = x * 2
```

### The `and`/`or` short-circuit

`a and b` evaluates `b` only if `a` is true. `a or b` evaluates `b` only if `a` is false.

```lucid
if has(cache, key) or expensiveLoad(key) { ... }
```

The short-circuit is part of the operator's semantics, not a `DEF`; it is fixed.

### Truthiness

`and`, `or`, and `not` accept values of any type and coerce them to truth:

- Non-nullable, non-fallible value: always true.
- `T?`: true unless `nil`.
- `T!`: true unless `err`.
- `T?!`: true unless `nil` or `err`.

The result is always `bool`. The coercion is a runtime check for the nullable/fallible cases and a compile-time constant for the non-nullable case.

See [Nullable, Fallible, and Combined Types](#nullable-fallible-and-combined-types) for the narrowing rules that interact with truthiness.

### Bitwise operators

`&`, `|`, `^`, `~`, `<<`, `>>` are integer-only. They are resolved through the `BINARY_OP` or `UNARY_OP` tables like any other operator.

`&` is also the reference type marker in type position. The parser distinguishes by syntactic position: in a type position, `&` introduces a reference type; in an expression position, `&` is the bitwise AND operator.

---

## Precedence and Associativity

Operators bind by the following table, highest to lowest:

| Level | Operators                   | Associativity |
| ----- | --------------------------- | ------------- |
| 7     | unary `-` `not` `~`         | right         |
| 6     | `*` `/` `%` `**`            | left          |
| 5     | `+` `-`                     | left          |
| 4     | `..` `..<`                  | left          |
| 3     | `==` `!=` `<` `<=` `>` `>=` | left          |
| 2     | `and`                       | left          |
| 1     | `or`                        | left          |
| 0     | `??`                        | left          |
| -1    | `\|>`                       | left          |

`..` binds tighter than comparison so range bounds can be arithmetic without parentheses, but looser than `+`/`-`/`*`/`/` so each bound is fully evaluated before the range is formed.

`??` is above `|>` and below `or`. `a ?? b |> f` parses as `(a ?? b) |> f`.

`|>` is lowest precedence; a pipeline extends as far right as possible.

---

## `??` Fallback

```
null_coalesce_expr ::= expr '??' expr
```

`x ?? y` evaluates to `x` if `x` is a value, and to `y` if `x` is `nil`, `err`, or both:

```lucid
let a int? = nil;
let b int = a ?? 0;                -- b is 0

let c int! = err;
let d int = c ?? -1;               -- d is -1

let e Vec2?! = nil;
let f Vec2 = e ?? Vec2 { x = 0.0, y = 0.0 };
```

The result type is the plain type `T`. `??` always resolves the sentinel.

### `??` on non-nullable values

`??` on a non-nullable value is legal but the right-hand side is dead code:

```lucid
let x int = 5;
let y int = x ?? 0;    -- y is 5; 0 is never used
```

The compiler may warn about this.

### `??` as a panic guard

`??` at a risky operation converts a runtime panic into a fallback:

```lucid
let a int = 10 / d;         -- PANIC if d == 0
let b int = 10 / d ?? -1;   -- fallback to -1 if d == 0
```

### `??` and narrowing

`??` does not narrow the left-hand side; it resolves it. After `x ?? y`, `x` is still the nullable/fallible type it was. The result of the whole expression is the plain type.

```lucid
let x int? = ...;
let y int = x ?? 0;    -- y is int
                       -- x is still int? on the next line
```

---

## Pipeline

```
pipeline_expr ::= expr { '|>' pipeline_step }
pipeline_step ::= expr [ '(' [ arg_list ] ')' '!' ]
```

A pipeline passes the result of one expression as an argument to the next step:

```lucid
[1, 2, 3]
    |> map<int, int>((x int) -> int { return x * 2; })!
    |> filter<int>((x int) -> bool { return x > 2; })!
    |> sum;
```

The pipeline executes left to right. Each step receives the upstream value and produces a new value.

### Step forms

A step is one of:

- **`expr`** — a function value with a single parameter. The upstream value is passed as the only argument.

  ```lucid
  [1, 2, 3] |> sum
  ```

- **`expr(args)!`** — a call with an argument pack. The upstream value is injected as the first argument; the remaining arguments fill the rest.

  ```lucid
  5 |> add(3)!    -- calls add(5, 3)
  ```

The `!` is mandatory for the second form. `f(args)` without `!` is not a valid pipeline step.

### Injection

The upstream value fills the first unfilled parameter of the step function:

```lucid
const add fn (a int, b int) -> int = { return a + b; };

5 |> add(3)!;    -- a := 5 (injected), b := 3 (explicit) → 8
```

For a curried function, the upstream value fills the next unfilled group:

```lucid
const clamp fn (lo int) fn (hi int) fn (v int) -> int = { ... };
const clamp0to100 fn (v int) -> int = clamp(0)(100);

42 |> clamp0to100;    -- → 42
150 |> clamp0to100;   -- → 100
```

### Anonymous functions as steps

A function literal is a valid step:

```lucid
5 |> (x int) -> int { return x * 2; };    -- → 10
```

### Chained pipelines

A pipeline may have many steps:

```lucid
const result string =
    42
    |> toStr
    |> (s string) -> string { return "value: " ++ s; };
```

Each `|>` extends the pipeline to the right.

### Step type checking

Each step's function type is checked against the upstream value's type:

- The upstream value must be assignable to the step's first parameter.
- The step's return type becomes the next step's upstream type.

A mismatch is a compile error at the step:

```
error: pipeline step expects 'int' but upstream value is 'string'
```

---

## Function Literals

```
func_literal ::= group { '->' unnamed_stage } '->' type block
group        ::= '(' [ param_list ] ')'
unnamed_stage ::= ( 'fn' | 'cls' ) unnamed_group
unnamed_group ::= '(' [ type_list ] ')'
```

A function literal is an anonymous function value:

```lucid
(x int) -> int { return x * 2; }
(a int, b int) -> int { return a + b; }
() -> unit { println("hello"); }
```

The leading group's parameters are named. Subsequent stages are type positions (no names) and carry `fn`/`cls` markers:

```lucid
(n int) -> cls (int) -> int {
    return (b int) -> int { return n + b; };
}
```

### Shape inference

A function literal's shape is inferred from whether its body captures anything:

- No captures → `fn`.
- Captures → `cls`.

The inferred shape is checked against the declared type at the binding site. See [The four-cell shape table](#the-four-cell-shape-table) for the four-cell table.

### Capture

A function literal captures variables from the enclosing scope:

- A variable that the body reads but does not write is captured by value (a snapshot).
- A variable that the body writes is captured by reference (shared storage).

The determination is per-variable, not per-literal.

```lucid
let f fn (n int) -> cls () -> int {
    let x int = 0;
    return () -> int {
        x = x + n;    -- x captured by reference, n captured by value
        return x;
    };
}
```

### What can be captured

Any value can be captured: primitives, structs, arrays, references, closures, `Deferred<T>`, etc. — with one exception.

`Deferred<T>` and `Thread<T>` cannot be captured. They are linear values; capturing them would allow two holders of the same pending operation. The rule is enforced at the capture site.

### Nested literals

A literal may contain another literal:

```lucid
let makeCounter = () -> cls () -> int {
    let n int = 0;
    return () -> int {
        n = n + 1;
        return n;
    };
};
```

The inner literal captures `n` from the outer literal's body. The outer literal's shape is `fn` if it captures nothing from *its* enclosing scope; the inner literal's shape is `cls` because it captures `n`.

### Literals as arguments and returns

A literal may be passed to a function:

```lucid
apply((x int) -> int { return x * 2; }, 5);
```

Or returned from a function:

```lucid
const makeDoubler fn () -> fn (int) -> int = {
    return (x int) -> int { return x * 2; };
};
```

---

## Range Expressions

```
range_expr ::= expr range_op expr
range_op   ::= '..' | '..<'
```

A range is a start value and an end value, with inclusive (`..`) or exclusive (`..<`) end:

```lucid
0..10       -- 0 through 10 inclusive
0..<10      -- 0 through 9
```

A range appears in three positions:

- As the iterable of a `for` loop (see [`for` — range iteration](#for-range-iteration)).
- As the bounds of a slice expression (see [Slice expressions](#slice-expressions)).
- As a `case` value in `switch` (see [Literal ranges](#literal-ranges)).

A range is not a standalone value. It has no type of its own and cannot be stored in a variable or passed to a function. Using a range outside these three positions is a compile error.

### Bounds

The bounds must be the same numeric type. The step (when present in a `for` loop) must also match. In a `switch` case, both bounds must be literals.

---

## Parenthesized Expressions

```
paren_expr ::= '(' expr ')'
```

A parenthesized expression groups a subexpression:

```lucid
(1 + 2) * 3
```

Parentheses do not introduce a tuple or any other type. A parenthesized expression is exactly its contents.

---

## The `DEF` Table

Every operator and named-call fact in the language is a `DEF` declaration. The `DEF` table is the collection of all `DEF`s visible to the program, partitioned by op kind and keyed by operand types.

### Structure

A `DEF` table has one entry per `DEF` declaration:

```
entry = (op_kind, symbol, operand_types) -> implementation
```

- `op_kind` — the kind of operation (`BINARY_OP`, `UNARY_OP`, `INDEX_GET`, etc.).
- `symbol` — the operator's spelling or the call's name (empty for index operations).
- `operand_types` — the parameter types after substitution.
- `implementation` — the target (`#host`, `#native`, `#builtin`, a named function, or an inline block).

### Population

The table is populated in three phases:

1. **Core scripts.** All `DEF`s declared in the core scripts are registered first. These provide the primitive operations: `+` on `int`, `toStr` on `string`, and so on.

2. **User module declarations.** Each user module's top-level `DEF`s are registered as the module is compiled.

3. **`satisfy` block expansion.** The `DEF`s inside `satisfy` blocks are registered as if they were top-level declarations in their enclosing module. A `DEF` inside a `satisfy` block is visible wherever a top-level `DEF` in that module would be visible.

### Lookup

A `DEF` lookup is a query against the table:

```
lookup(op_kind, symbol, operand_types) -> implementation | not_found
```

The lookup prefers the most specific match:

1. **Exact concrete match.** A `DEF` whose operand types are exactly the query's operand types. If more than one exact match exists, it is a redeclaration error.
2. **Generic match.** A `DEF` with generic parameters whose constraints are satisfied by the query's operand types. If more than one generic match applies, it is an ambiguity error.
3. **No match.** The operation is a compile error.

### Concrete beats generic

A concrete `DEF` shadows a generic `DEF` for the same operation and symbol when the operand types match exactly:

```lucid
DEF BINARY_OP '==' <T : Eq> (a T, b T) -> bool = { ... };
DEF BINARY_OP '==' (a Vec2, b Vec2) -> bool = { ... };

let v Vec2 = ...;
let b bool = v == w;    -- uses the concrete Vec2 DEF
```

The generic `DEF` is still available for other types; the concrete one wins only for its operand types.

### Ambiguity

If two generic `DEF`s both match a query and neither is more specific, it is an ambiguity error:

```lucid
DEF CALL 'describe' <T : Numeric> (v T) -> string = { ... };
DEF CALL 'describe' <T : Stringable> (v T) -> string = { ... };

-- if SomeType satisfies both Numeric and Stringable:
describe(someValue);    -- ERROR: ambiguous
```

The error names both candidates and suggests a concrete `DEF` for the type:

```
error: ambiguous call to 'describe'
   = note: two DEFs match:
     DEF CALL 'describe' <T : Numeric> (v T) -> string
     DEF CALL 'describe' <T : Stringable> (v T) -> string
   = help: add a concrete DEF for the operand type
```

There is no precedence between traits. Two traits that both match are an error, not a tie broken by some ordering.

### Redeclaration

Two `DEF`s with the same op kind, symbol, and operand types are a redeclaration error:

```lucid
DEF BINARY_OP '+' (a int, b int) -> int = #native(add_i32);
DEF BINARY_OP '+' (a int, b int) -> int = #native(add_i32_v2);    -- ERROR
```

A user's `DEF` may not override a core-script `DEF` of the same signature. The error is reported at the second declaration.

---

## The Standard `OpKind` Set

The core script declares the standard operation kinds:

```lucid
TYPE OpKind = #host(OpKind)
FN registerOpKind (name string) -> OpKind = #host(register_op_kind)

const BINARY_OP  OpKind = registerOpKind("BINARY_OP");
const UNARY_OP   OpKind = registerOpKind("UNARY_OP");
const INDEX_GET  OpKind = registerOpKind("INDEX_GET");
const INDEX_SET  OpKind = registerOpKind("INDEX_SET");
const CALL       OpKind = registerOpKind("CALL");
```

### `BINARY_OP`

A binary operation between two operands. The symbol is the operator's spelling:

```lucid
DEF BINARY_OP '+' (a int, b int) -> int = #native(add_i32);
DEF BINARY_OP '==' (a Vec2, b Vec2) -> bool = { ... };
DEF BINARY_OP 'and' (a bool, b bool) -> bool = #native(and_bool);
```

The symbol may be any operator token: `+`, `-`, `*`, `/`, `%`, `**`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `&`, `|`, `^`, `<<`, `>>`, `and`, `or`.

### `UNARY_OP`

A unary operation on one operand. The symbol is the operator's spelling:

```lucid
DEF UNARY_OP '-' (v int) -> int = #native(neg_i32);
DEF UNARY_OP 'not' (v bool) -> bool = #native(not_bool);
DEF UNARY_OP '~' (v int) -> int = #native(bitnot_i32);
```

The symbol may be `-`, `not`, or `~`.

### `INDEX_GET`

A read access on a container. The signature is `(container, index) -> value`:

```lucid
DEF INDEX_GET (a [*]T, i uint) -> T = #builtin(array_get);
DEF INDEX_GET (m &Map<K, V>, k K) -> V? = #builtin(map_get);
```

The first parameter is the container; the second is the index or key. The return type is the element type (possibly wrapped in `?` if the key might be absent).

The symbol slot is empty for `INDEX_GET`.

### `INDEX_SET`

A write access on a container. The signature is `(container, index, value) -> unit`:

```lucid
DEF INDEX_SET (a &[*]T, i uint, v T) = #builtin(array_set);
DEF INDEX_SET (m &Map<K, V>, k K, v V) = #builtin(map_set);
```

The first parameter is the container (by reference, since the operation mutates); the second is the index or key; the third is the value to store. The return type is `unit`.

The symbol slot is empty for `INDEX_SET`.

### `CALL`

A named-call operation. The symbol is the call's name:

```lucid
DEF CALL 'toStr' (v int) -> string = #builtin(int_to_str);
DEF CALL 'toStr' (v Vec2) -> string = { ... };
DEF CALL 'describe' (v int) -> string = { ... };
```

The first parameter is the receiver; the remaining parameters are the call's arguments. A call site `name(args)` resolves through the `CALL` table, keyed on the argument types.

### Adding new op kinds

A core script may register a new op kind:

```lucid
const MEMBER_GET OpKind = registerOpKind("MEMBER_GET");
```

Once registered, `DEF MEMBER_GET` declarations are legal, and the corresponding operations are available. The compiler must know how to lower the new op kind; in practice, adding an op kind is a compiler change, not a core-script change. The `registerOpKind` mechanism exists so that the compiler's op-kind set is not baked into the parser, not so that user scripts can add new syntax.

---

## Overload Resolution

Overload resolution is the process that maps a call site or operator use to a `DEF` implementation. The rules are uniform across all op kinds.

### The resolution algorithm

For a use `op(sym, args)`:

1. **Collect candidates.** All `DEF`s in the table with matching `op_kind` and `symbol`.
2. **Filter by arity.** Candidates whose parameter count does not match `args` are discarded.
3. **Filter by type compatibility.** Candidates whose parameter types are not satisfiable by the argument types are discarded. A generic candidate's constraints are checked against the argument types.
4. **Rank.** Exact concrete matches beat generic matches. If exactly one candidate remains, it wins. If multiple remain, it is an ambiguity error.

The check is done at the call site; the compiler does not defer to runtime.

### Generic substitution

For a generic `DEF`, the compiler substitutes the type arguments and checks the constraints:

```lucid
DEF BINARY_OP '!=' <T : Eq> (a T, b T) -> bool = { ... };
```

For a call `a != b` where `a` and `b` are both `Vec2`:

1. The candidate is `DEF BINARY_OP '!=' <T : Eq>`.
2. The compiler binds `T := Vec2`.
3. It checks that `Vec2` satisfies `Eq`.
4. If so, the candidate applies.

The specialization `!=` for `Vec2` is then compiled as a concrete function and used at the call site.

### The `CALL` table

`CALL` resolutions are keyed by the call's name and the argument types. The `CALL` table is a sub-table of the `DEF` table, partitioned by symbol:

```lucid
DEF CALL 'toStr' (v int) -> string = #builtin(int_to_str);
DEF CALL 'toStr' (v string) -> string = { return v; };
DEF CALL 'toStr' (v Vec2) -> string = { ... };
```

A call `toStr(x)` looks up `CALL 'toStr'` with the argument `x`'s type. If the type is `int`, the first `DEF` matches; if `Vec2`, the third.

### The `INDEX_GET` and `INDEX_SET` tables

Index operations are keyed by the container's type only (the index type is part of the signature). A `DEF INDEX_GET (a [*]T, i uint) -> T` and a `DEF INDEX_GET (m &Map<K, V>, k K) -> V?` are in the same table, disambiguated by the container's type.

### No user-defined overloading by name

Lucid has no user-defined overloading by function name. A name declared with `const` or `FN` has exactly one implementation. Two declarations with the same name are a redeclaration error, not an overload set.

The overload mechanism applies only to `DEF` declarations, where the operation is keyed by op kind and symbol. This keeps name resolution unambiguous: a plain name always refers to exactly one declaration; a `DEF` operation is resolved through the `DEF` table.

### Implicit coercion in resolution

The only implicit conversion is `fn → cls`. In overload resolution, a candidate whose parameter is `cls (T) -> U` is compatible with an argument of type `fn (T) -> U` (the argument is coerced). The reverse is not allowed.

No other implicit conversion exists. An argument of type `int` is not compatible with a parameter of type `uint`, even though both are integers.

---

## `satisfy` and Constraint Checking

A `satisfy` block asserts that a type satisfies a trait. The block is checked at its declaration site, and its conformance is used at constraint sites.

### What `satisfy` checks

A `satisfy Trait for Type` block is checked against the trait's clauses:

- **`FIELD` clauses.** The type must have a field with the clause's name and type. Field checking is implicit; the `satisfy` block does not list fields.
- **`REQUIRE` clauses.** The type must provide a `DEF` of the required op kind, symbol, and signature. The `DEF` may be in the `satisfy` block, at the top level of the same module, in another module reachable from `main`, or in a core script.

A trait with both clause kinds requires both field and operation conformance.

### `satisfy` block contents

A `satisfy` block contains only `DEF` declarations:

```lucid
satisfy Numeric for Vec2 {
    DEF BINARY_OP '+' (a Vec2, b Vec2) -> Vec2 = { ... };
    DEF BINARY_OP '-' (a Vec2, b Vec2) -> Vec2 = { ... };
    DEF BINARY_OP '*' (a Vec2, b Vec2) -> Vec2 = { ... };
    DEF BINARY_OP '/' (a Vec2, b Vec2) -> Vec2 = { ... };
}
```

Field conformance is not declared in the block. A field-only trait has an empty block:

```lucid
satisfy Named for Player { }
```

### Missing members

If a `satisfy` block does not provide a `DEF` for a trait's `REQUIRE` clause, it is a compile error:

```
error: 'satisfy Numeric for Vec2' does not provide a definition for
       BINARY_OP '/' (a Vec2, b Vec2) -> Vec2
   = note: 'Numeric' requires this operation
```

### Extra members

A `DEF` in a `satisfy` block that is not required by the trait is allowed. It is a normal declaration in the block's scope and is visible program-wide (subject to the module's export rules).

### Generic `satisfy`

A generic `satisfy` block covers all instantiations of a generic trait for a generic type:

```lucid
trait Container<T> {
    FIELD value T;
    FIELD count uint;
}

struct Box<T> { value T; count uint; }

satisfy Container<T> for Box<T> { }
```

The generic parameters `<T>` are shared between the trait application and the type. Every parameter that appears on either side must be listed.

When the compiler checks a constraint `<C : Container<int>>` at a call site, it looks up the `satisfy Container<T> for Box<T>` block and substitutes `T := int`.

### Constraint checking at use sites

A constraint `<T : TraitName>` at a function declaration or a generic `TYPE` is checked at each use:

```lucid
const first<T : Container<int>> fn (c T) -> int? = {
    if c.count == 0 { return nil; }
    return c.value;
};

first(someBoxOfInt);    -- OK if Box<int> satisfies Container<int>
first(someString);      -- ERROR if string does not satisfy Container<int>
```

The constraint is resolved by looking up `satisfy Container<int> for X` in the table. If no `satisfy` block exists for the concrete type, the constraint fails.

### Inheritance of constraints

A trait's parent traits are constraints on the trait:

```lucid
trait Ord : Eq {
    REQUIRE BINARY_OP '<' (self Self, rhs Self) -> bool;
}
```

A type satisfying `Ord` must also satisfy `Eq`. The compiler checks the parent constraints first. If the type fails a parent, the diagnostic points at the parent:

```
error: 'Vec2' satisfies 'Ord' but does not satisfy 'Eq'
   = note: 'Ord : Eq' requires 'Eq'
   = help: add 'satisfy Eq for Vec2 { ... }'
```

### Compiler-inserted `satisfy` blocks

The compiler inserts two `satisfy` blocks automatically:

- `satisfy StructType for X { }` for every struct declaration.
- `satisfy EnumType for X { }` for every enum declaration.

`StructType` and `EnumType` are compiler-provided traits with no clauses. They are used by the standard library's `toStr` fallback to dispatch on struct and enum types.

### Interaction with operator resolution

A `DEF` inside a `satisfy` block is usable as an operator as soon as the block is checked. For example:

```lucid
satisfy Eq for Vec2 {
    DEF BINARY_OP '==' (a Vec2, b Vec2) -> bool = { ... };
}

let a Vec2 = ...;
let b Vec2 = ...;
let eq bool = a == b;    -- resolves through the DEF in the satisfy block
```

The `==` is usable because the `DEF` is registered. Whether `Vec2` formally satisfies `Eq` is a separate question — it does, because the `satisfy` block asserts it — but the operator itself is a fact that is available regardless.

### No implicit conformance

A type does not satisfy a trait unless a `satisfy` block asserts it (or the compiler inserts one for `StructType`/`EnumType`). A type that happens to have the fields a trait requires does not automatically satisfy it:

```lucid
trait Named {
    FIELD name string;
}

struct Player {
    name string;
    health int;
}

-- no satisfy block: Player does not satisfy Named
let p Player = ...;
let n string = p.name;    -- OK: field access
```

To use `Player` where `Named` is required, add a `satisfy` block:

```lucid
satisfy Named for Player { }
```

Without it, `first<Player>(players)` where `first` requires `Named` is a compile error.

---

## The `Stringable` Trait

`toStr` ([`toStr`](#tostr)) is the stringification mechanism itself — the `CALL 'toStr'` `DEF` family that every call to `toStr(v)` resolves through. `Stringable` does not add a second mechanism; it is a trait that lets a `toStr` be *required*, by asking for exactly the operation that already exists:

```lucid
trait Stringable {
    REQUIRE CALL 'toStr' (self Self) -> string;
}
```

A user type satisfies it with a `satisfy` block, which supplies the concrete `DEF` and asserts conformance together:

```lucid
satisfy Stringable for Vec2 {
    DEF CALL 'toStr' (v Vec2) -> string = {
        return "(" ++ toStr(v.x) ++ ", " ++ toStr(v.y) ++ ")";
    };
}
```

The `DEF` declared in the block is an ordinary concrete `DEF` for `toStr` on `Vec2` — nothing about it is special to `Stringable`. It overrides the compiler's struct fallback ([Struct and enum `toStr`](#struct-and-enum-tostr)) for `Vec2` by the standard `CALL` overload resolution ([The `toStr` fallback chain](#the-tostr-fallback-chain)).

### Using `Stringable` as a constraint

A function that needs to *require* a `toStr` to exist — rather than merely calling `toStr` and accepting whatever resolves — constrains on `Stringable`:

```lucid
const logValue<T : Stringable> fn (label string, v T) = {
    println(label ++ ": " ++ toStr(v));
};
```

The constraint checks for a `satisfy Stringable for T` block ([`satisfy` and Constraint Checking](#satisfy-and-constraint-checking)), not for a `toStr` `DEF`'s mere existence in the table. If `T`'s only `toStr` is the compiler's struct/enum fallback and no `satisfy Stringable` block was written for it, the constraint fails — even though `toStr(v)` itself would work fine outside this constrained context.

### `Stringable` vs. the compiler fallback

The compiler's `toStr` fallback (`struct_to_str`/`enum_to_str`, [Struct and enum `toStr`](#struct-and-enum-tostr)) handles structs and enums automatically, via the `StructType`/`EnumType` marker traits every struct and enum satisfies by construction ([`StructType` and `EnumType`](#structtype-and-enumtype)). A type with no `Stringable` conformance still has a working `toStr` — it just resolves to the struct-walk or variant-name form. `Stringable` is not about whether the `toStr` is hand-written versus generated; it is about whether the type has an explicit `satisfy Stringable` block asserting it. A type could, in principle, `satisfy Stringable` with a `DEF` that just delegates to `#builtin(struct_to_str)` — that would satisfy the constraint without providing a genuinely custom form. In practice `Stringable` is used for custom forms, but the trait itself only checks conformance, not novelty.

A function that accepts any `toStr` — custom, fallback, or otherwise — doesn't need the constraint at all:

```lucid
const logValue<T> fn (label string, v T) = {
    println(label ++ ": " ++ toStr(v));    -- toStr works on any T
};
```

`<T : Stringable>` is the narrower of the two: it requires formal conformance, not just a resolvable `toStr`.

---

## `OpKind` Resolution

The `op_kind` in a `DEF` declaration is an identifier resolved against `OpKind` values. The resolution happens in Sema, not the parser.

### Resolution rules

An `op_kind` identifier resolves by the same rules as any other identifier: local scope, module scope, imports. The value must be of type `OpKind`. A name that resolves to a non-`OpKind` value is a compile error.

```lucid
DEF BINARY_OP '+' (...)    -- OK: BINARY_OP is an OpKind
DEF INTEGER '+' (...)       -- ERROR: INTEGER is not an OpKind
DEF unknown_op '+' (...)    -- ERROR: unknown_op is not defined
```

### The core-script declarations

The standard op kinds are declared in the core script:

```lucid
const BINARY_OP  OpKind = registerOpKind("BINARY_OP");
const UNARY_OP   OpKind = registerOpKind("UNARY_OP");
const INDEX_GET  OpKind = registerOpKind("INDEX_GET");
const INDEX_SET  OpKind = registerOpKind("INDEX_SET");
const CALL       OpKind = registerOpKind("CALL");
```

The `registerOpKind` function is a `#host` binding. It returns the `OpKind` value for the given name, creating it if it does not exist. Calling `registerOpKind("BINARY_OP")` twice returns the same `OpKind` value.

### Identity

An `OpKind` value's identity is its name. Two `OpKind` values are equal iff their names are equal. This means:

- `registerOpKind("BINARY_OP")` called twice returns equal values.
- `registerOpKind("BINARY_OP")` and `registerOpKind("UNARY_OP")` return distinct values.
- A `DEF BINARY_OP` and a `DEF UNARY_OP` are in different partitions of the `DEF` table.

The name is the identity; the order of registration and the specific `OpKind` value's internal representation are irrelevant.

### Why `op_kind` is data, not a keyword

Making `op_kind` an identifier has two consequences:

- The parser has no list of valid op kinds. Adding a new one is a core-script change, not a parser change.
- A misspelled op kind is a name-resolution error, not a parse error. The diagnostic is better: `undefined operation kind 'BINARY_OPP'` with a suggestion, rather than `unexpected token`.

### Adding new op kinds

A core script may register a new op kind:

```lucid
const MEMBER_GET OpKind = registerOpKind("MEMBER_GET");
```

Once registered, `DEF MEMBER_GET` declarations are legal. The compiler must know how to lower a use of the new op kind; this is a compiler change even though the registration is a script change. The mechanism exists so that the parser's vocabulary is not the op-kind set, not so that user scripts can invent new operator syntax.

---

## The Fiber Model

Lucid's concurrency is **cooperative** and runs on a **single VM thread**. There is one thread of execution in the VM at any moment; the scheduler switches between fibers at defined suspension points. Script code cannot be preempted, so script data cannot be raced by construction.

### Fibers

A **fiber** is a suspendable unit of execution. It has its own call stack and its own local storage. When a fiber suspends at an `await`, the scheduler saves its state and runs another fiber. When the awaited operation completes, the scheduler resumes the fiber at the point it suspended.

The VM's main execution is itself a fiber. When the program starts, the VM creates one fiber for the entry point and runs it. Any `start` or `spawn` from within a fiber creates a new fiber.

### The scheduler

The scheduler runs on the VM thread. It maintains:

- **A ready queue** of fibers that can run.
- **A waiting set** of fibers suspended on a `Deferred<T>` that has not resolved.
- **A completion queue** of resolved operations, populated by the host or by the engine.

The scheduler's main loop, run once per engine tick:

1. Drain the completion queue into the waiting set, waking any fibers whose deferred operations have resolved.
2. Run ready fibers until each suspends or completes, up to a per-tick time budget.
3. Advance any timers whose deadlines have passed.

If the time budget is exceeded, the scheduler returns to the engine, and resumes on the next tick. This keeps the VM's work bounded per frame, so a long-running script does not stall the game loop.

### Suspension points

A fiber suspends at an `await`. It runs from the last suspension point to the next one without interruption. Between suspension points, the fiber has exclusive access to all script data — no other fiber can run, so no other fiber can see a partial update.

```lucid
const async update fn (world &World) = {
    world.counter = world.counter + 1;    -- atomic with respect to other fibers
    await someDeferred;                   -- suspend point
    world.counter = world.counter + 1;    -- atomic again
};
```

The two `world.counter + 1` operations cannot interleave with another fiber's operations, because they run between suspension points. This is what makes cooperative concurrency safe by construction.

### The one crossing point

The scheduler's completion queue is the only place where engine-side threads interact with the VM. Engine threads (I/O workers, physics workers, etc.) push results into the queue via a thread-safe channel; the scheduler drains the queue on the VM thread. No engine thread ever touches script data directly.

This is the firewall between true parallelism and the VM. Everything inside the VM is single-threaded; everything outside the VM that produces a result does so by pushing into the queue.

### The `#host` boundary

Scripts can request engine-side parallelism through an `#host` function that returns a `Deferred<T>`:

```lucid
FN host_async<T> (work #hostFn) -> Deferred<T> = #host(host_run_async);
```

The `#host` function schedules work on an engine worker thread and returns a `Deferred<T>`. The scheduler treats the returned deferred like any other: the fiber awaits it, and the scheduler wakes the fiber when the worker thread pushes the result into the completion queue.

The script never sees threads. It sees deferreds.

---

## `async`, `spawn`, `start`, `await`

Four keywords introduce and consume concurrency. Their grammar:

```
async_marker ::= 'async'
spawn_stmt   ::= 'spawn' call_expr ';'
start_stmt   ::= 'start' IDENTIFIER type '=' call_expr ';'
await_stmt   ::= 'await' IDENTIFIER { ',' IDENTIFIER } ';'
             | 'await' 'all' '(' IDENTIFIER { ',' IDENTIFIER } ')' ';'
             | 'await' 'any' '(' IDENTIFIER { ',' IDENTIFIER } ')' ';'
```

### `async`

`async` marks a function as a fiber entry point. It appears in the declaration's header, between the keyword and the function name:

```lucid
const async fetchUser (id int) -> User = {
    ...
};

FN async readFile (path string) -> string = #host(read_file_async);
```

Only `async` functions may be `spawn`ed, `start`ed, or `await`ed. A call to an `async` function must use one of the three forms; a bare call is a compile error.

An `async` function may or may not contain an `await`. A function that never suspends can still be marked `async` for uniformity with a family of related functions; the compiler may warn about the unnecessary marking but does not reject it.

### `spawn`

`spawn f(args);` runs `f(args)` on a new fiber and discards the result. `f` must be an `async` function.

```lucid
spawn playSound("jump.wav");
spawn logEvent("player jumped");
```

`spawn` never binds a value. There is no `spawn x = ...` form; to hold a result, use `start`.

### `start`

`start d T = f(args);` runs `f(args)` on a new fiber and binds a `Deferred<T>` to `d`. `f` must be an `async` function; `T` is the function's return type.

```lucid
start d User = fetchUser(7);
start e [*]Texture = fetchTextures("level1");
```

The `Deferred<T>` is a handle to the fiber's result. It must be consumed by `await` on every path out of its scope.

A `let` or `const` keyword may precede the binding:

```lucid
start const d User = fetchUser(7);    -- immutable binding
start let   d User = fetchUser(7);    -- mutable binding
```

The `const`/`let` choice affects whether the binding can be reassigned after it is consumed. It does not affect the `Deferred<T>`'s semantics.

### `await`

`await d;` suspends the current fiber until the deferred `d` resolves, then narrows `d` from `Deferred<T>` to `T` for the rest of the scope.

```lucid
start d User = fetchUser(7);
-- ... other work ...
await d;
-- d is User here
println(d.name);
```

Multiple variables may be awaited in one statement:

```lucid
await a, b, c;
-- a, b, c are all resolved
```

The multiple-variable form waits for all of them.

### The three call forms

An `async` function `f` may be called in exactly three ways:

- **`spawn f(args);`** — fire-and-forget. The result is discarded.
- **`start d T = f(args);`** — held handle. The result is a `Deferred<T>`.
- **`await start f(args);`** — run synchronously from the caller's perspective. A `Deferred<T>` is created, awaited, and the resolved `T` is the result of the expression.

A bare call `f(args)` is a compile error:

```
error: call to 'async' function 'fetchUser' must specify how it runs
   --> main.luc:5:17
    |
  5 |     let u User = fetchUser(7);
    |                 ^^^^^^^^^
    |
   = note: 'fetchUser' is marked 'async' and may suspend
   = help: pick one of:
     spawn fetchUser(7);                       -- fire-and-forget
     start d User = fetchUser(7);              -- held handle
     let u User = await start fetchUser(7);    -- run and block
```

### The `await start` composition

`await start f(args)` is not a separate keyword; it is the composition of `start` and `await`. A `start` expression produces a `Deferred<T>` that is immediately awaited:

```lucid
let u User = await start fetchUser(7);
```

The compiler synthesizes an anonymous binding for the deferred:

```lucid
start __tmp User = fetchUser(7);
await __tmp;
let u User = __tmp;
```

The user writes the composed form; the compiler handles the intermediate.

### `await all` and `await any`

`await all(a, b, c)` is parser sugar for `await a; await b; await c;`. All deferreds are consumed; all values become available.

`await any(a, b, c)` suspends until one of the deferreds resolves, returns that one's value, and **consumes all of them**. The non-winners remain pending on the scheduler; they have no handle after the `await any`.

```lucid
start a int = fetch1();
start b int = fetch2();
start c int = fetch3();

let first int = await any(a, b, c);
-- a, b, c are all consumed; the first to resolve provided the value
```

The consumed-all rule is a consequence of `Deferred<T>` being a linear value: every deferred in the `await any` expression is consumed by the await. There is no way to await the losers afterward.

---

## `Deferred<T>` — Linear Value Rules

`Deferred<T>` is the handle a `start` produces. It is a **linear value**: it must be consumed by exactly one `await` before the enclosing scope exits.

### The rules

1. **`Deferred<T>` is not `T`.** A `Deferred<T>` cannot be used where a `T` is expected. Field access, method calls, arithmetic, and parameter passing all require a `T`, not a `Deferred<T>`.

2. **`await` is the only path from `Deferred<T>` to `T`.** There is no implicit unwrap, no `.value` field, no conversion.

3. **`await` consumes.** A deferred can be awaited at most once. A second `await` on the same binding is a compile error.

4. **A live deferred must be consumed on every path out of its scope.** Reaching scope exit with an unresolved `Deferred<T>` is a compile error.

5. **A deferred may only exist as a local or a parameter.** It cannot be stored in a struct field, in an array element, or captured by a closure.

6. **A deferred may not be captured by a closure literal.** This includes capture from an enclosing scope and capture of the enclosing function's own parameter.

Rules 5 and 6 close every indirect path a deferred could take into a context where its consumption cannot be tracked.

### Flow sensitivity

The consumed status of a deferred follows the same flow-sensitive analysis as `T?` and `T!` narrowing. After `await d;`, `d` is consumed on every path through the await. If the await is inside an `if`, `d` is consumed only on the path that takes the `if`; a subsequent `await d` after the `if` is legal only if `d` was not consumed on every path reaching it.

```lucid
start d User = fetchUser(7);

if someCondition {
    await d;
    println(d.name);
}
-- d may or may not be consumed here, depending on someCondition

await d;    -- OK: d is consumed on the 'if' path, but not on the fall-through
```

```lucid
start d User = fetchUser(7);

if someCondition {
    await d;
} else {
    await d;
}
-- d is consumed on both paths

await d;    -- ERROR: d is consumed on every path reaching here
```

### Scope exit

A `Deferred<T>` that reaches the end of its scope without being consumed is a compile error:

```lucid
const async f fn () -> unit = {
    start d User = fetchUser(7);
    -- ...
    -- no await
};
-- ERROR: 'd' is a live Deferred<User> at scope exit
```

The error:

```
error: 'd' is a live Deferred<User> at scope exit
   --> main.luc:3:11
    |
  3 |     start d User = fetchUser(7);
    |           ^
    |
   = note: a Deferred<T> must be consumed by 'await' before its scope exits
   = help: either await the deferred, or use 'spawn' to discard the result
```

The fix is either to await the deferred, or to use `spawn` (which discards the result by design).

### Errors from the check

Two errors are produced by this analysis:

- **Unconsumed at scope exit.** A live deferred reaches scope exit.
- **Double consumption.** A deferred is awaited a second time on the same path.

Both errors are flow-sensitive. The compiler reports them at the offending statement.

### The `start` statement is a binding

`start d T = f(args);` introduces a fresh binding `d` of type `Deferred<T>`. It is not an assignment to an existing variable. There is no form `start d = f(args);` where `d` already exists; the deferred binding is always new.

This is the same rule as `let` and `const`: a binding declaration introduces a new name, it does not reuse an existing one.

---

## Cancellation

A deferred may be cancelled:

```lucid
FN cancel<T> (d &Deferred<T>) = #builtin(cancel_deferred);
FN isReady<T> (d &Deferred<T>) -> bool = #builtin(deferred_ready);
```

### `cancel`

`cancel(d)` requests cancellation of the fiber producing `d`. The fiber is marked cancelled; the scheduler stops it at the next suspension point or lets it run to completion, depending on the engine's cancellation policy.

`cancel` does not consume `d`. A cancelled deferred is still a deferred; it has the same linear-value rules.

### `await` after `cancel`

`await` on a cancelled deferred is a **runtime panic**:

```lucid
start d User = fetchUser(7);
cancel(d);
await d;    -- PANIC: awaited a cancelled deferred
```

The panic terminates the current fiber with a diagnostic. There is no try/catch; the design does not provide a way to catch the panic and continue.

The rule: **cancel and do not await; or await and do not cancel.** The two are mutually exclusive. The compiler does not enforce this statically (cancel is a runtime operation on a runtime value), so the panic is the enforcement.

### `isReady`

`isReady(d)` returns `true` if the deferred's fiber has resolved. It does not consume the deferred; it is a poll.

```lucid
if isReady(d) {
    await d;    -- completes immediately
}
```

`isReady` is an optimization for polling without blocking. It is never required for correctness: an `await` on a non-ready deferred suspends the fiber until it is ready.

### What cancellation does not do

`cancel` does not:

- Free the deferred's storage. The `Deferred<T>` binding still exists and still must be consumed (by `await`, which will panic) or dropped (by scope exit, which is a compile error if the deferred is still live).
- Interrupt the fiber mid-operation. The fiber stops at its next suspension point, not immediately.
- Provide a value for the deferred. A cancelled deferred never resolves.

To handle a cancelled deferred without panicking, the user must not await it. Use `isReady` to check, or drop the binding entirely:

```lucid
start d User = fetchUser(7);
if shouldCancel {
    cancel(d);
    -- do not await; the binding is dropped at scope exit, which is an error...
}
```

Wait — this is a conflict. If `cancel(d)` does not consume `d`, and the user does not `await d`, then `d` is a live deferred at scope exit, which is a compile error (rule 4). So the user cannot cancel-and-drop either.

This is a real gap in the design. Let me flag it and propose a fix.

### The cancel-and-discard problem

The rules as stated produce a contradiction: after `cancel(d)`, the user must either `await d` (which panics) or let `d` go out of scope unconsumed (which is a compile error). Neither is acceptable.

Two fixes:

**Fix A — `cancel` consumes.** Change the rule: `cancel(d)` consumes the deferred. After cancellation, `d` is no longer available; a subsequent `await d` is a compile error (not a runtime panic). The user can cancel-and-drop without triggering the scope-exit check.

```lucid
start d User = fetchUser(7);
cancel(d);    -- consumes d
-- d is no longer live
```

Under this fix, `cancel` and `await` are mutually exclusive at the type level. The compiler knows which one happened and rejects the other. This is cleaner: the "cancel then await panics" rule becomes "cancel then await is a compile error," which is the same shape as "await twice is a compile error."

**Fix B — a `cancel` return type of `unit` plus an explicit discard.** Add a `discard(d)` function that consumes a deferred without awaiting it:

```lucid
start d User = fetchUser(7);
cancel(d);
discard(d);    -- consumes d without producing a value
```

This is more explicit but adds a new function for a rare case.

**Fix A is cleaner.** Change `cancel` to consume the deferred, so `cancel(d)` and `await d` are alternative ways to consume a `Deferred<T>`. The compiler enforces that exactly one is used.

I'll note this in the review points. The section above describes the current design; Fix A is the recommendation.

---

## Fibers and `spawn` Without a Handle

`spawn f(args);` runs `f` on a fiber and discards the result. There is no handle, no binding, and no `await`.

```lucid
spawn playSound("jump.wav");
spawn logEvent("player jumped");
```

The spawned fiber runs until it completes or suspends. If it suspends, the scheduler resumes it when its awaited operations complete. If it completes without suspending, it runs to completion within the current tick.

The user has no way to observe the fiber's progress, cancel it, or wait for it. This is deliberate: `spawn` is fire-and-forget.

To observe a spawn's progress, use `start` and hold the deferred.

### What happens to a spawned fiber's error

A spawned fiber that panics (division by zero, indexing out of bounds, or an explicit `error(msg)` call) terminates the fiber with a diagnostic. The panic does not propagate to the spawning fiber; the spawned fiber is independent.

If the engine has configured a panic handler, the panic is reported through it. Otherwise, the panic terminates the fiber silently (or logs to the engine's error channel).

### What happens to spawned fibers on program exit

When the program's entry fiber completes, the VM terminates. Any spawned fibers that have not completed are discarded. Their in-progress operations are abandoned.

This is the same as any fire-and-forget mechanism: the spawner does not wait, and the VM does not wait either.

---

## Concurrency and the Standard Library

The core script declares the standard concurrency types and functions:

```lucid
TYPE Deferred<T> = #host(LucidDeferred)

FN isReady<T> (d &Deferred<T>) -> bool = #builtin(deferred_ready)
FN cancel<T>  (d &Deferred<T>)       = #builtin(cancel_deferred)

-- Sleep: a fiber-level yield until a deadline
const async sleep fn (seconds float) -> unit = #host(host_sleep);

-- Engine-parallel work
FN host_async<T> (work #hostFn) -> Deferred<T> = #host(host_run_async);
```

`host_async` is the bridge to engine-side parallelism. A script wraps a C++ function as a `#hostFn` and calls `host_async(work)` to run it on a worker thread:

```lucid
@[host_only]
const computePhysics fn (worldId int, dt float) -> PhysicsResult = {};

const async stepWorld fn (worldId int, dt float) -> PhysicsResult = {
    let d Deferred<PhysicsResult> = host_async(computePhysics(worldId, dt));
    return await d;
};
```

The script never spawns a thread. It asks the engine to run a specific C++ function on a worker thread, gets a `Deferred<T>`, and awaits it like any other deferred. The engine keeps full control of its thread pool.

---

## Restrictions Summary

The concurrency restrictions, in one place:

| #   | Rule                                                                                                                   |
| --- | ---------------------------------------------------------------------------------------------------------------------- |
| 1   | `async` marks a function as a fiber entry point. Only `async` functions may be `spawn`ed, `start`ed, or `await`ed.     |
| 2   | `spawn f(args)` requires `f` to be `async`.                                                                            |
| 3   | `start d T = f(args)` requires `f` to be `async`; `T` is the inner type; `d` is a fresh binding of type `Deferred<T>`. |
| 4   | `await d` requires `d` to be a live `Deferred<T>`. It consumes `d` and narrows it to `T`.                              |
| 5   | A bare call to an `async` function is a compile error.                                                                 |
| 6   | A live `Deferred<T>` must be consumed on every path out of its scope.                                                  |
| 7   | A `Deferred<T>` may not be stored in a struct field or array element, or captured by a closure.                        |
| 8   | A `Deferred<T>` may not be awaited twice on the same path.                                                             |
| 9   | `cancel(d)` does not consume `d` (under the current design).                                                           |
| 10  | `await` on a cancelled deferred is a runtime panic.                                                                    |

Rule 9 is the one flagged in [The cancel-and-discard problem](#the-cancel-and-discard-problem). If Fix A is adopted, rule 9 becomes "`cancel(d)` consumes `d`" and rule 10 becomes "`await` on a cancelled deferred is a compile error."

---

## The Core Scripts

The standard library is written in Lucid and loaded before any user code. It is organized into modules, each a separate file, imported as needed.

| Module        | Purpose                                                                                       |
| ------------- | --------------------------------------------------------------------------------------------- |
| `core`        | Primitives, operators, foundational types, `toStr`, `print`, `error`/`warn`, memory builtins. |
| `core.map`    | `Map<K, V>` and its operations.                                                               |
| `core.array`  | Array operations and functional helpers.                                                      |
| `core.string` | String manipulation.                                                                          |
| `core.math`   | Arithmetic utilities, trigonometry, random.                                                   |
| `core.io`     | Console and file I/O via the engine's VFS.                                                    |
| `core.fn`     | Function composition helpers.                                                                 |

The core scripts use the same frames as user code. Nothing in them is special at the syntax level; their only privilege is that they are loaded first, so their declarations are available before user code compiles.

### What lives where

**`core`** declares the primitive types, the basic operators (`+`, `-`, `==`, etc. for primitives), the `OpKind` values, `Option<T>`, `Fallible<T>`, `Both<T>`, `Deferred<T>`, `Weak<T>`, the `toStr` `DEF` family, `print`/`println`, `error`/`warn`, and the `#builtin` declarations for memory, strings, closures, references, and fibers.

**`core.map`** declares `Map<K, V>` and its operations. `Map` is a host-backed type; its storage is on the C++ side.

**`core.array`** declares the array operations (`push`, `pop`, `insert`, etc.) and the functional helpers (`map`, `filter`, `reduce`, `sort`). It does not declare an `Array<T>` type; the arrays `[*]T`, `[_]T`, `[N]T` are boot-level, and the operations are free functions.

**`core.string`** declares string manipulation: `split`, `trim`, `find`, `format`, and the string conversion functions.

**`core.math`** declares arithmetic utilities: `abs`, `min`, `max`, `floor`, `ceil`, `round`, `sqrt`, `pow`, `sin`, `cos`, `tan`, and the random number functions.

**`core.io`** declares console and file I/O: `print`, `println`, `readLine`, `readFile`, `writeFile`, and the engine's VFS access.

**`core.fn`** declares function composition helpers: `compose`, `identity`, `const`.

### Importing the core scripts

A user file imports a core module by name:

```lucid
import core.io as io
import core.math as math

@[export] const main (args [*]string) -> int = {
    io::println("sqrt(2) = " ++ toStr(math::sqrt(2.0)));
    return 0;
};
```

The `core` module itself is implicitly imported by every file. Its types (`int`, `string`, `bool`, `Option`, `Fallible`, `Deferred`, `Weak`, etc.) are always in scope; user code does not need to import them.

---

## Primitive Types and Their Operations

The `core` module declares the primitive types and their operations.

### The primitive types

```lucid
TYPE bool   = #host(bool)
TYPE char   = #host(char)
TYPE string = #host(string)

TYPE byte   = #host(int8)
TYPE short  = #host(int16)
TYPE int    = #host(int32)
TYPE long   = #host(int64)

TYPE ubyte  = #host(uint8)
TYPE ushort = #host(uint16)
TYPE uint   = #host(uint32)
TYPE ulong  = #host(uint64)

TYPE float  = #host(float)
TYPE double = #host(double)

TYPE unit   = #host(unit)
```

The sized names (`int8`, `uint32`, etc.) are aliases:

```lucid
TYPE int8   = byte
TYPE int16  = short
TYPE int32  = int
TYPE int64  = long
TYPE uint8  = ubyte
TYPE uint16 = ushort
TYPE uint32 = uint
TYPE uint64 = ulong
```

### The basic operators

Each primitive type has the operators its semantics support. For `int`:

```lucid
DEF BINARY_OP '+' (a int, b int) -> int = #native(add_i32);
DEF BINARY_OP '-' (a int, b int) -> int = #native(sub_i32);
DEF BINARY_OP '*' (a int, b int) -> int = #native(mul_i32);
DEF BINARY_OP '/' (a int, b int) -> int = #native(div_i32);
DEF BINARY_OP '%' (a int, b int) -> int = #native(rem_i32);
DEF BINARY_OP '==' (a int, b int) -> bool = #native(eq_i32);
DEF BINARY_OP '!=' (a int, b int) -> bool = #native(ne_i32);
DEF BINARY_OP '<' (a int, b int) -> bool = #native(lt_i32);
DEF BINARY_OP '<=' (a int, b int) -> bool = #native(le_i32);
DEF BINARY_OP '>' (a int, b int) -> bool = #native(gt_i32);
DEF BINARY_OP '>=' (a int, b int) -> bool = #native(ge_i32);

DEF UNARY_OP '-' (v int) -> int = #native(neg_i32);
DEF UNARY_OP '~' (v int) -> int = #native(bitnot_i32);
```

Similar sets exist for `byte`, `short`, `long`, `ubyte`, `ushort`, `uint`, `ulong`, `float`, `double`, and `bool`.

### Comparison and logical operators

`bool`:

```lucid
DEF BINARY_OP '==' (a bool, b bool) -> bool = #native(eq_bool);
DEF BINARY_OP '!=' (a bool, b bool) -> bool = #native(ne_bool);
DEF BINARY_OP 'and' (a bool, b bool) -> bool = #native(and_bool);
DEF BINARY_OP 'or'  (a bool, b bool) -> bool = #native(or_bool);
DEF UNARY_OP  'not' (v bool) -> bool = #native(not_bool);
```

`string`:

```lucid
DEF BINARY_OP '==' (a string, b string) -> bool = #builtin(str_eq);
DEF BINARY_OP '!=' (a string, b string) -> bool = { return not (a == b); };
DEF BINARY_OP '<' (a string, b string) -> bool = #builtin(str_lt);
DEF BINARY_OP '<=' (a string, b string) -> bool = { return a < b or a == b; };
DEF BINARY_OP '>' (a string, b string) -> bool = { return b < a; };
DEF BINARY_OP '>=' (a string, b string) -> bool = { return b <= a; };
```

`char`:

```lucid
DEF BINARY_OP '==' (a char, b char) -> bool = #native(eq_char);
DEF BINARY_OP '!=' (a char, b char) -> bool = #native(ne_char);
DEF BINARY_OP '<' (a char, b char) -> bool = #native(lt_char);
DEF BINARY_OP '<=' (a char, b char) -> bool = #native(le_char);
DEF BINARY_OP '>' (a char, b char) -> bool = #native(gt_char);
DEF BINARY_OP '>=' (a char, b char) -> bool = #native(ge_char);
```

### Derived operators

The remaining comparison operators are derived generically:

```lucid
DEF BINARY_OP '!=' <T : Eq>  (a T, b T) -> bool = { return not (a == b); };
DEF BINARY_OP '<=' <T : Ord> (a T, b T) -> bool = { return a < b or a == b; };
DEF BINARY_OP '>'  <T : Ord> (a T, b T) -> bool = { return b < a; };
DEF BINARY_OP '>=' <T : Ord> (a T, b T) -> bool = { return b <= a; };
```

Concrete `DEF`s for these operators on specific types take precedence over the generics.

---

## The Trait Catalog

The `core` module declares the standard traits.

### `Eq` and `Ord`

```lucid
trait Eq {
    REQUIRE BINARY_OP '==' (self Self, rhs Self) -> bool;
}

trait Ord : Eq {
    REQUIRE BINARY_OP '<' (self Self, rhs Self) -> bool;
}
```

### Arithmetic traits

```lucid
trait Add {
    REQUIRE BINARY_OP '+' (self Self, rhs Self) -> Self;
}

trait Sub {
    REQUIRE BINARY_OP '-' (self Self, rhs Self) -> Self;
}

trait Mul {
    REQUIRE BINARY_OP '*' (self Self, rhs Self) -> Self;
}

trait Div {
    REQUIRE BINARY_OP '/' (self Self, rhs Self) -> Self;
}

trait Rem {
    REQUIRE BINARY_OP '%' (self Self, rhs Self) -> Self;
}

trait Neg {
    REQUIRE UNARY_OP '-' (self Self) -> Self;
}

trait Numeric : Add, Sub, Mul, Div, Neg {}
trait Integral : Numeric, Rem, Eq, Ord {}
```

### `Stringable`

```lucid
trait Stringable {
    REQUIRE CALL 'toStr' (self Self) -> string;
}
```

This is a constraint form, not a mechanism: it does not itself produce a string, and `toStr` ([`toStr`](#tostr)) works identically whether or not `Stringable` is ever mentioned. `Stringable` exists solely so a function can write `<T : Stringable>` and require formal, `satisfy`-asserted conformance rather than accepting whatever `toStr` resolves to (see [The `Stringable` Trait](#the-stringable-trait-1)).

### `StructType` and `EnumType`

```lucid
trait StructType {}
trait EnumType {}
```

The compiler inserts `satisfy StructType for X { }` for every struct and `satisfy EnumType for X { }` for every enum. The traits have no clauses; they exist as markers so that `toStr`'s fallbacks can be constrained.

### `Indexable` and `IndexableMut`

```lucid
trait Indexable<K, V> {
    REQUIRE INDEX_GET (self Self, k K) -> V;
}

trait IndexableMut<K, V> {
    REQUIRE INDEX_GET (self Self, k K) -> V;
    REQUIRE INDEX_SET (self Self, k K, v V);
}
```

These traits describe containers that support index access. They are used by generic algorithms that operate on any indexable container.

### The primitive conformances

The core module satisfies the standard traits for the primitive types:

```lucid
satisfy Eq for int {
    DEF BINARY_OP '==' (a int, b int) -> bool = #native(eq_i32);
}

satisfy Ord for int {
    DEF BINARY_OP '<' (a int, b int) -> bool = #native(lt_i32);
}

satisfy Numeric for int {
    DEF BINARY_OP '+' (a int, b int) -> int = #native(add_i32);
    DEF BINARY_OP '-' (a int, b int) -> int = #native(sub_i32);
    DEF BINARY_OP '*' (a int, b int) -> int = #native(mul_i32);
    DEF BINARY_OP '/' (a int, b int) -> int = #native(div_i32);
    DEF UNARY_OP  '-' (v int) -> int = #native(neg_i32);
}

satisfy Integral for int {
    DEF BINARY_OP '%' (a int, b int) -> int = #native(rem_i32);
}
```

The same pattern is repeated for each numeric type and for `string` (which satisfies `Eq` and `Ord` but not the arithmetic traits).

---

## `Option<T>`, `Fallible<T>`, `Both<T>`

The nullable, fallible, and combined types are declared in `core`:

```lucid
struct Option<T> {
    has   bool;
    value T;
}

struct Fallible<T> {
    ok    bool;
    value T;
}

struct Both<T> {
    tag   int;    -- 0 = nil, 1 = value, 2 = err
    value T;
}
```

The compiler lowers `T?` to `Option<T>`, `T!` to `Fallible<T>`, and `T?!` to `Both<T>`. The user writes `T?` and `T!`; the struct wrapping is internal.

### Construction

`nil` and `err` construct the sentinel states:

```lucid
let a Option<int> = nil;
let b Fallible<int> = err;
```

A present value wraps automatically when assigned to a nullable or fallible binding:

```lucid
let c int? = 5;    -- wraps 5 in Option<int> { has = true, value = 5 }
```

### Narrowing

Narrowing is a compiler feature, not a function. The comparisons `== nil`, `!= nil`, `== err`, `!= err` are recognized by the type checker. The `if let` form binds the wrapped value.

### The `??` fallback

`??` is a compiler-lowered form. It reads the `has`/`ok`/`tag` field and returns either the value or the fallback:

```lucid
let a int? = nil;
let b int = a ?? 0;
```

`b` is `0`. The lowering reads `a.has`, returns `a.value` if true, or evaluates the fallback otherwise.

---

## `toStr`

The `toStr` `DEF` family provides stringification for every type. It is declared in `core`.

### Primitive `toStr`

```lucid
DEF CALL 'toStr' (v int)    -> string = #builtin(int_to_str);
DEF CALL 'toStr' (v uint)   -> string = #builtin(uint_to_str);
DEF CALL 'toStr' (v long)   -> string = #builtin(int_to_str);    -- same handler, wider type
DEF CALL 'toStr' (v float)  -> string = #builtin(float_to_str);
DEF CALL 'toStr' (v double) -> string = #builtin(float_to_str);
DEF CALL 'toStr' (v bool)   -> string = #builtin(bool_to_str);
DEF CALL 'toStr' (v char)   -> string = #builtin(char_to_str);
DEF CALL 'toStr' (v string) -> string = { return v; };
```

### Struct and enum `toStr`

```lucid
DEF CALL 'toStr' <T : StructType> (v T) -> string = #builtin(struct_to_str);
DEF CALL 'toStr' <T : EnumType>   (v T) -> string = #builtin(enum_to_str);
```

The struct walker iterates the fields and formats each as `fieldName: value`. The enum walker formats the variant as `EnumName.VariantName` for integer enums, or `EnumName.Variant` for payload enums.

### User overrides

A user type provides its own `toStr` by declaring a concrete `DEF CALL 'toStr'` for itself, most conveniently inside a `satisfy Stringable` block ([The `Stringable` Trait](#the-stringable-trait-1)), which provides the `DEF` and asserts formal conformance together:

```lucid
satisfy Stringable for Vec2 {
    DEF CALL 'toStr' (v Vec2) -> string = {
        return "(" ++ toStr(v.x) ++ ", " ++ toStr(v.y) ++ ")";
    };
}
```

The `DEF` in the block is a concrete `DEF` for `toStr` on `Vec2`, indistinguishable from one written at top level. Either form takes precedence over the generic struct fallback ([Struct and enum `toStr`](#struct-and-enum-tostr)) by ordinary overload resolution; only the `satisfy` form also grants formal `Stringable` conformance.

### The `toStr` fallback chain

When `toStr(v)` is called, the compiler resolves through the `CALL` table with ordinary overload resolution — there is a single lookup, not a `Stringable`-then-fallback sequence, since `Stringable`'s `REQUIRE` names the very same `CALL 'toStr'` operation rather than a separate one:

1. If a concrete `DEF CALL 'toStr'` exists for the argument's type — written at top level or inside a `satisfy Stringable` block, both produce the same kind of `DEF` — use it.
2. Otherwise, if the type satisfies `StructType`, use `struct_to_str`.
3. Otherwise, if the type satisfies `EnumType`, use `enum_to_str`.

`Stringable` itself never appears in this resolution — it is a constraint checked separately, at a call site that writes `<T : Stringable>` ([Using `Stringable` as a constraint](#using-stringable-as-a-constraint)), not a step `toStr` itself consults.

### `toStr` for container types

```lucid
DEF CALL 'toStr' <T> (v [*]T) -> string = {
    let s string = "[";
    for i uint, x T in v {
        if i > 0 { s = s ++ ", "; }
        s = s ++ toStr(x);
    }
    return s ++ "]";
};

DEF CALL 'toStr' <T> (v [_]T) -> string = {
    -- same as [*]T
};

DEF CALL 'toStr' <T> (d Deferred<T>) -> string = { return "<deferred>"; };
DEF CALL 'toStr' <T> (w Weak<T>) -> string = { return "<weak>"; };
```

These use the recursive `toStr` dispatch; `toStr(x)` inside the loop resolves on `x`'s type.

---

## `print` and `println`

The `core.io` module declares the output functions:

```lucid
FN write (s string) = #host(host_write);

const print<T>   (v T) = { write(toStr(v)); };
const println<T> (v T) = { write(toStr(v)); write("\n"); };
```

`print` and `println` are plain generic functions. They call `toStr` on their argument, which dispatches through the `CALL` table.

### No variadic printing

`println` takes exactly one argument:

```lucid
println("hello");
println(42);
println(toStr(x) ++ " and " ++ toStr(y));
```

There is no variadic form. To print several values, concatenate them with `++` and `toStr`, or use string interpolation:

```lucid
println("x = \(toStr(x)), y = \(toStr(y))");
```

### `print` without newline

`print` writes without a trailing newline:

```lucid
print("loading");
print(".");
print(".");
println(" done");
```

### Other output

The `core.io` module may also provide:

```lucid
const printErr<T>   (v T) = { writeErr(toStr(v)); };
const printlnErr<T> (v T) = { writeErr(toStr(v)); writeErr("\n"); };
```

These write to the error channel. They are used by `warn` and by the panic handler.

---

## `error` and `warn`

The `core` module declares the two diagnostic functions:

```lucid
FN panic_str (msg string) = #builtin(panic_str);
FN warn_str  (msg string) = #builtin(warn_str);

const error (msg string) = { panic_str(msg); };
const warn  (msg string) = { warn_str(msg); };
```

`error(msg)` terminates the current fiber with a diagnostic. The message is printed, along with a stack trace.

`warn(msg)` prints the message to the warning channel and returns.

Both are ordinary functions; they are not keywords. The names `error` and `warn` are exported by `core` and are available to user code.

### `error` vs `err`

`error(msg)` is a panic. `err` is a sentinel value for `T!`.

```lucid
-- panic: terminates the fiber
if b == 0 { error("division by zero"); }

-- fallible: returns a sentinel that the caller must narrow
if b == 0 { return err; }
```

The choice: use `err` when the failure is a value the caller can handle; use `error` when the failure is a programmer error or an unrecoverable condition.

### `warn` does not terminate

`warn(msg)` writes the message and returns. It does not unwind or terminate.

```lucid
if value < 0 {
    warn("negative value: \(toStr(value))");
    value = 0;
}
```

---

## `Map<K, V>`

The `core.map` module declares `Map<K, V>`.

```lucid
TYPE Map<K, V> = #host(LucidMap)

FN map_new<K, V>    () -> Map<K, V>          = #builtin(map_new);
FN map_len<K, V>    (m &Map<K, V>)           -> uint = #builtin(map_len);
FN map_has<K, V>    (m &Map<K, V>, k K)      -> bool = #builtin(map_has);
FN map_remove<K, V> (m &Map<K, V>, k K)      -> bool = #builtin(map_remove);
FN map_keys<K, V>   (m &Map<K, V>)           -> [*]K = #builtin(map_keys);
FN map_values<K, V> (m &Map<K, V>)           -> [*]V = #builtin(map_values);

DEF INDEX_GET (m &Map<K, V>, k K) -> V? = #builtin(map_get);
DEF INDEX_SET (m &Map<K, V>, k K, v V) = #builtin(map_set);
```

### Construction

```lucid
let scores Map<string, int> = map_new<string, int>();
```

There is no map literal. The map is constructed empty and populated with `INDEX_SET`:

```lucid
scores["alice"] = 10;
scores["bob"]   = 20;
```

### Reading

`m[k]` returns `V?` — `nil` if the key is absent:

```lucid
if let v int = scores["alice"] {
    println("alice: " ++ toStr(v));
}
```

`map_has(m, k)` returns a `bool` without returning the value:

```lucid
if map_has(scores, "alice") {
    ...
}
```

### Iterating

A `for` loop iterates key/value pairs:

```lucid
for k string, v int in scores {
    println(k ++ ": " ++ toStr(v));
}

for k string, _ in scores {
    println(k);
}

for _, v int in scores {
    println(toStr(v));
}
```

The first binding is the key type, the second is the value type.

### Removing

`map_remove(m, k)` removes the entry and returns `true` if it existed:

```lucid
if map_remove(scores, "alice") {
    println("removed alice");
}
```

### A helper for building from pairs

The `core.map` module also declares a helper for building a map from an array of key/value pairs:

```lucid
struct Entry<K, V> {
    key   K;
    value V;
}

const map_of<K, V> fn (entries [_]Entry<K, V>) -> Map<K, V> = {
    let m Map<K, V> = map_new<K, V>();
    for _, e Entry<K, V> in entries {
        m[e.key] = e.value;
    }
    return m;
};
```

Usage:

```lucid
let scores Map<string, int> = map_of<string, int>([
    Entry<string, int> { key = "alice", value = 10 },
    Entry<string, int> { key = "bob",   value = 20 },
]);
```

The helper is a core-script function, not a grammar construct. It is provided because the "build from a fixed set of pairs" pattern is common; if a project does not need it, it can simply not be used.

---

## Array Operations

The `core.array` module declares the array operations. The arrays themselves (`[*]T`, `[_]T`, `[N]T`) are boot-level; the operations are free functions.

```lucid
FN array_len<T>    (a [_]T)               -> uint = #builtin(array_len);
FN array_push<T>   (a &[*]T, v T)         = #builtin(array_push);
FN array_pop<T>    (a &[*]T)              -> T?   = #builtin(array_pop);
FN array_insert<T> (a &[*]T, i uint, v T) = #builtin(array_insert);
FN array_remove<T> (a &[*]T, i uint)      = #builtin(array_remove);
FN array_resize<T> (a &[*]T, n uint)      = #builtin(array_resize);
FN array_clear<T>  (a &[*]T)              = #builtin(array_clear);
```

### Basic operations

```lucid
let xs [*]int = [1, 2, 3];
array_push(xs, 4);                   -- xs = [1, 2, 3, 4]
let last int? = array_pop(xs);       -- last = 4, xs = [1, 2, 3]
array_insert(xs, 0, 0);              -- xs = [0, 1, 2, 3]
array_remove(xs, 0);                 -- xs = [1, 2, 3]
array_resize(xs, 5);                 -- xs = [1, 2, 3, 0, 0]
array_clear(xs);                     -- xs = []
```

### `array_pop` returns `T?`

`array_pop` returns `T?` because the array might be empty:

```lucid
if let v int = array_pop(xs) {
    println("popped: " ++ toStr(v));
} else {
    println("array was empty");
}
```

### Functional helpers

```lucid
const array_map<T, U> fn (xs [_]T, f cls (T) -> U) -> [*]U = {
    let result [*]U = [];
    for _, x T in xs {
        array_push(result, f(x));
    }
    return result;
};

const array_filter<T> fn (xs [_]T, pred cls (T) -> bool) -> [*]T = {
    let result [*]T = [];
    for _, x T in xs {
        if pred(x) {
            array_push(result, x);
        }
    }
    return result;
};

const array_reduce<T, U> fn (xs [_]T, seed U, f cls (U, T) -> U) -> U = {
    let acc U = seed;
    for _, x T in xs {
        acc = f(acc, x);
    }
    return acc;
};

const array_find<T> fn (xs [_]T, pred cls (T) -> bool) -> T? = {
    for _, x T in xs {
        if pred(x) { return x; }
    }
    return nil;
};

const array_any<T> fn (xs [_]T, pred cls (T) -> bool) -> bool = {
    for _, x T in xs {
        if pred(x) { return true; }
    }
    return false;
};

const array_all<T> fn (xs [_]T, pred cls (T) -> bool) -> bool = {
    for _, x T in xs {
        if not pred(x) { return false; }
    }
    return true;
};
```

### `array_sort`

```lucid
const array_sort<T> fn (xs [*]T, cmp fn (T, T) -> int) = {
    -- in-place sort, calling cmp
};
```

`array_sort` takes a comparator of type `fn (T, T) -> int`. The comparator is `fn` (not `cls`) because sort is a hot path and the comparator should not capture.

The comparator returns a negative value if the first argument sorts before the second, zero if equal, and a positive value otherwise.

```lucid
array_sort(xs, (a int, b int) -> int { return a - b; });    -- ascending
array_sort(xs, (a int, b int) -> int { return b - a; });    -- descending
```

### Pipelines

The array functions compose with `|>`:

```lucid
const result [*]string =
    [1, 2, 3, 4, 5]
    |> array_filter<int>((x int) -> bool { return x > 2; })!
    |> array_map<int, string>((x int) -> string { return toStr(x); })!;
```

---

## String Operations

The `core.string` module declares string manipulation.

```lucid
FN strLen    (s string) -> uint = #builtin(str_len);
FN strEq     (a string, b string) -> bool = #builtin(str_eq);
FN strConcat (a string, b string) -> string = #builtin(str_concat);
FN strSlice  (s string, from uint, to uint) -> string = #builtin(str_slice);
FN strFromPtr (p &uint8, len uint) -> string = #builtin(str_from_ptr);

const split fn (s string, sep string) -> [*]string = { ... };
const trim  fn (s string) -> string = { ... };
const find  fn (s string, needle string) -> uint? = { ... };
const contains fn (s string, needle string) -> bool = { ... };
const startsWith fn (s string, prefix string) -> bool = { ... };
const endsWith   fn (s string, suffix string) -> bool = { ... };
const replace    fn (s string, from string, to string) -> string = { ... };
```

### `split`

```lucid
let parts [*]string = split("a,b,c", ",");
-- parts = ["a", "b", "c"]
```

### `trim`

```lucid
let s string = trim("  hello  ");
-- s = "hello"
```

### `find`

```lucid
if let i uint = find("hello world", "world") {
    println("found at " ++ toStr(i));    -- 6
}
```

### `++` and string concatenation

The `++` operator is desugared to `strConcat`:

```lucid
let s string = "hello, " ++ "world";
```

### String conversion

The `core.string` module (or `core`) declares the conversion functions:

```lucid
const stringFromInt    fn (n int) -> string = { return toStr(n); };
const stringFromFloat  fn (f float) -> string = { return toStr(f); };
const stringFromBool   fn (b bool) -> string = { return toStr(b); };
const intFromString    fn (s string) -> int! = { ... };
const floatFromString  fn (s string) -> float! = { ... };
```

`intFromString` and `floatFromString` return a fallible type: parsing can fail.

---

## Math Operations

The `core.math` module declares arithmetic utilities.

```lucid
FN sqrt (x float) -> float = #native(sqrt_f32);
FN pow  (base float, exp float) -> float = #native(pow_f32);
FN sin  (x float) -> float = #native(sin_f32);
FN cos  (x float) -> float = #native(cos_f32);
FN tan  (x float) -> float = #native(tan_f32);
FN floor (x float) -> float = #native(floor_f32);
FN ceil  (x float) -> float = #native(ceil_f32);
FN round (x float) -> float = #native(round_f32);
FN abs   (x float) -> float = #native(abs_f32);

const min<T : Ord> fn (a T, b T) -> T = { return if a < b ?? a else b; };
const max<T : Ord> fn (a T, b T) -> T = { return if a > b ?? a else b; };

const PI  float = 3.141592653589793;
const TAU float = 6.283185307179586;
const E   float = 2.718281828459045;
```

### Random

```lucid
FN random     () -> float = #host(host_random);
FN randomInt  (lo int, hi int) -> int = #host(host_random_int);
FN seedRandom (seed uint64) = #host(host_seed_random);
```

### Using math

```lucid
import core.math as math

let r float = math::sqrt(2.0);
let angle float = math::PI / 4.0;
let s float = math::sin(angle);
```

---

## `Weak<T>` and Cycle Handling

The `core` module declares `Weak<T>`:

```lucid
TYPE Weak<T> = #host(LucidWeak)

FN weak<T>    (v &T)      -> Weak<T> = #builtin(weak);
FN upgrade<T> (w Weak<T>) -> &T?     = #builtin(upgrade);
FN strongCount<T> (v &T) -> uint = #builtin(strong_count);
FN weakCount<T>   (w Weak<T>) -> uint = #builtin(weak_count);
```

### Using `Weak<T>` to break cycles

A back edge in an ownership graph is stored as `Weak<T>`:

```lucid
struct TreeNode {
    value    int;
    children [*]TreeNode;
    parent   Weak<TreeNode>?;
}

let root TreeNode = TreeNode { value = 0, children = [], parent = nil };
let child TreeNode = TreeNode { value = 1, children = [], parent = nil };
child.parent = weak(root);
```

The parent edge does not keep `root` alive. When the last strong reference to `root` drops, the tree is freed, and `upgrade(child.parent)` returns `nil`.

### `upgrade`

`upgrade` returns a strong reference if the referent is alive:

```lucid
if let p &TreeNode = upgrade(child.parent) {
    println("parent value: " ++ toStr(p.value));
} else {
    println("parent is gone");
}
```

### Cycle warnings

The compiler warns on obvious same-scope cycles:

```
warning: 'a.next' and 'b.prev' form a reference cycle
   = help: consider making one edge 'Weak<T>'
```

The warning is best-effort. Many cycles cannot be detected statically.

---

## `Deferred<T>` and Concurrency Support

The `core` module declares `Deferred<T>` and the concurrency helpers:

```lucid
TYPE Deferred<T> = #host(LucidDeferred)

FN isReady<T> (d &Deferred<T>) -> bool = #builtin(deferred_ready);
FN cancel<T>  (d &Deferred<T>) = #builtin(cancel_deferred);

const async sleep fn (seconds float) = #host(host_sleep);
FN host_async<T> (work #hostFn) -> Deferred<T> = #host(host_run_async);
```

### `sleep`

`sleep` suspends the current fiber for the given number of seconds:

```lucid
const async tick fn () = {
    while true {
        doWork();
        await sleep(1.0);
    }
};
```

The fiber suspends and is resumed by the scheduler when the deadline passes.

### `host_async`

`host_async(work)` schedules a host function on an engine worker thread and returns a `Deferred<T>`:

```lucid
@[host_only]
const computePhysics fn (worldId int, dt float) -> PhysicsResult = {};

const async step fn (worldId int, dt float) -> PhysicsResult = {
    let d Deferred<PhysicsResult> = host_async(computePhysics(worldId, dt));
    return await d;
};
```

The script never touches threads; it asks the engine to run the function and awaits the result.

---

## `core.fn` — Function Composition

The `core.fn` module declares composition helpers.

```lucid
const identity<T> fn (v T) -> T = { return v; };

const constFn<T, U> fn (v T) -> fn (U) -> T = {
    return (x U) -> T { return v; };
};

const compose2<A, B, C> fn (f cls (A) -> B, g cls (B) -> C) -> cls (A) -> C = {
    return (x A) -> C { return g(f(x)); };
};

const compose3<A, B, C, D> fn (f cls (A) -> B, g cls (B) -> C, h cls (C) -> D) -> cls (A) -> D = {
    return (x A) -> D { return h(g(f(x))); };
};
```

`compose2` and `compose3` are alternatives to `|>` when a named composed function is wanted:

```lucid
import core.fn as fn

const process fn (raw string) -> bool =
    fn::compose3(validate, transform, render);

let result bool = process(input);
```

The pipeline form is preferred for inline use; the composition helpers are for reusable function values.

---

## `core.io` — Console and File I/O

The `core.io` module declares console and file I/O.

```lucid
FN write    (s string) = #host(host_write);
FN writeErr (s string) = #host(host_write_err);

const print<T>    (v T) = { write(toStr(v)); };
const println<T>  (v T) = { write(toStr(v)); write("\n"); };
const printErr<T> (v T) = { writeErr(toStr(v)); };
const printlnErr<T> (v T) = { writeErr(toStr(v)); writeErr("\n"); };

FN readLine () -> string = #host(host_read_line);

FN readFile  (path string) -> string! = #host(host_read_file);
FN writeFile (path string, contents string) = #host(host_write_file);
FN fileExists (path string) -> bool = #host(host_file_exists);
```

### Console

```lucid
import core.io as io

io::println("hello");
io::print("enter your name: ");
let name string = io::readLine();
io::println("hello, " ++ name);
```

### File I/O

```lucid
import core.io as io

let contents string! = io::readFile("config.txt");
if contents == err {
    io::printlnErr("could not read config");
    return;
}
io::println(contents);
```

The file I/O is routed through the engine's VFS, so paths are resolved relative to the game's content root, not the OS filesystem.

---

## The Foreign Function Interface

Lucid interoperates with C and C++ through `@[foreign("C")]` declarations. The ABI is C; C++ is reached through `extern "C"` wrappers.

### The `@[foreign("C")]` attribute

A function declared `@[foreign("C")]` is implemented in C:

```lucid
@[foreign("C")]
FN malloc (size uint64) -> &uint8? = #host(malloc);
```

The attribute names the ABI. `"C"` is the only valid value; there is no `@[foreign("C++")]`.

The declaration is an ordinary `FN` with an `#host` target. The attribute tells the compiler that the symbol's calling convention and type mapping follow C rules.

### The `@[link(...)]` attribute

A declaration may name the library or file it links against:

```lucid
@[foreign("C"), link("m")]
FN sqrt (x float) -> float = #host(sqrt);

@[foreign("C"), link("vendor/lib/simd.c")]
FN simd_dot (a &float, b &float, n uint64) -> float = #host(simd_dot);
```

`@[link(...)]` accepts one or more strings. Each string is either:

- A library name (`"m"`, `"opengl"`, `"pthread"`) — passed to the linker as `-l<name>`.
- A source or object file path (`"vendor/lib/file.c"`, `"build/helper.o"`) — compiled and linked as part of the build.

A declaration may have multiple link targets:

```lucid
@[foreign("C"), link("vendor/math/fast.c", "vendor/math/lut.c", "m")]
FN fastSin (x float) -> float = #host(fastSin);
```

### Type mapping

C types map to Lucid types as follows:

| C type                | Lucid type                   |
| --------------------- | ---------------------------- |
| `int`                 | `int32`                      |
| `unsigned int`        | `uint32`                     |
| `long`                | `int64`                      |
| `unsigned long`       | `uint64`                     |
| `size_t`              | `uint64`                     |
| `float`               | `float`                      |
| `double`              | `double`                     |
| `char *`              | `&uint8`                     |
| `void *`              | `&uint8`                     |
| `T *`                 | `&T`                         |
| `T *` (may be `NULL`) | `&T?`                        |
| `void` (return)       | omitted (defaults to `unit`) |

A `char *` is not a Lucid `string`. It is a pointer to bytes; the conversion to a Lucid string is done with `strFromPtr`.

### Nullability

The Lucid declaration is the sole nullability contract. The compiler does not parse C headers:

```lucid
-- programmer asserts this never returns NULL
@[foreign("C")]
FN getGlobalState () -> &State = #host(getGlobalState);

-- programmer knows this may return NULL
@[foreign("C")]
FN findUser (id int32) -> &User? = #host(findUser);
```

An unannotated pointer return defaults to non-nullable (`&T`). Use `&T?` only when the C function may return `NULL`.

### Forbidden return types

A foreign function must not return a `&T` (a Lucid reference) directly. The rule from [References and `Weak<T>`](#references-and-weakt) applies: a Lucid reference is a managed pointer with a refcount; C's memory does not participate in Lucid's refcount table. Returning a `&T` from a foreign function would produce a reference with no managed backing.

The valid return types are:

- An owned value (a primitive, a struct, an enum).
- A host-managed pointer (`&T` where the pointer is from a Lucid allocator, not C's).

For C's memory, use a pointer type and convert on the Lucid side:

```lucid
@[foreign("C")]
FN c_malloc (size uint64) -> &uint8 = #host(c_malloc);

-- the result is a Lucid-managed reference to C-allocated memory
-- the caller must pass it back to C for freeing
@[foreign("C")]
FN c_free (p &uint8) = #host(c_free);
```

### C++ interop

C++ is not called directly. It is reached through an `extern "C"` wrapper:

```cpp
// kernel_wrapper.cpp
#include "kernel.hpp"

extern "C" {
    Kernel* kernel_create(int config) {
        return new Kernel(config);
    }

    void kernel_destroy(Kernel* self) {
        delete self;
    }

    int kernel_run(Kernel* self, float* data, int len) {
        return self->run(data, len);
    }
}
```

```lucid
@[foreign("C"), link("kernel_wrapper.cpp", "kernel")]
FN kernel_create (config int32) -> &uint8? = #host(kernel_create);

@[foreign("C"), link("kernel_wrapper.cpp", "kernel")]
FN kernel_destroy (self &uint8) = #host(kernel_destroy);

@[foreign("C"), link("kernel_wrapper.cpp", "kernel")]
FN kernel_run (self &uint8, data &float, len int32) -> int32 = #host(kernel_run);
```

The C++ object is an opaque `&uint8` on the Lucid side. Lucid has no knowledge of its layout, ownership, or lifetime.

### Exporting Lucid functions to C

A Lucid function may be exported to C with `@[export, foreign("C")]`:

```lucid
@[export, foreign("C")]
const add fn (a int32, b int32) -> int32 = {
    return a + b;
};
```

`@[export]` makes the symbol visible to the linker; `@[foreign("C")]` forces the C ABI on the boundary. Both attributes are required.

### Memory safety at the boundary

Lucid's memory guarantees end at the foreign boundary. The caller is responsible for:

- Not passing a scope-arena address to C that outlives the call.
- Not mixing allocators (`#alloc`-ed memory freed by C, or C-allocated memory freed by Lucid).
- Passing buffer sizes explicitly.
- Not calling `free` on Lucid-managed memory.

There is no compiler check for these; the design treats C interop as an unsafe boundary that the user is responsible for.

---

## Attributes

Attributes are compiler directives attached to declarations. They are a closed set.

### Syntax

```
attribute_list ::= '@[' attr { ',' attr } ']'
attr           ::= IDENTIFIER [ '(' attr_arg { ',' attr_arg } ')' ]
attr_arg       ::= STRING_LIT | INT_LIT | FLOAT_LIT | BOOL_LIT | IDENTIFIER
```

An attribute list precedes the declaration it modifies. Multiple attributes may share a list:

```lucid
@[export, foreign("C")]
const add fn (a int32, b int32) -> int32 = { return a + b; };
```

### The attribute set

| Attribute              | Valid on               | Meaning                                |
| ---------------------- | ---------------------- | -------------------------------------- |
| `@[export]`            | top-level declarations | Visible outside the file               |
| `@[foreign("C")]`      | function declarations  | Implemented in a foreign ABI           |
| `@[link("name", ...)]` | declarations           | Link against native libraries or files |
| `@[deprecated("msg")]` | any declaration        | Compiler warning at use sites          |
| `@[inline]`            | function declarations  | Inlining hint                          |
| `@[noinline]`          | function declarations  | Prevent inlining                       |
| `@[opaque]`            | struct fields          | No Lucid-side access                   |
| `@[host_only]`         | declarations           | Callable only from C++                 |

### Attribute semantics

**`@[export]`.** The declaration is visible outside its module. Without it, a declaration is private to the file. `@[export]` is top-level only; it is not allowed inside a block.

**`@[foreign("C")]`.** The function is implemented in C. The body must be empty (the declaration is `= #host(...)`). The ABI is C.

**`@[link("name", ...)]`.** The declaration links against the named libraries or files. Each string is a library name or a file path. Multiple strings may be given.

**`@[deprecated("msg")]`.** Use of the declaration produces a warning with the given message. The attribute is informational.

**`@[inline]`.** A hint to the compiler to inline calls to the function. The compiler may ignore it.

**`@[noinline]`.** A hint to the compiler not to inline calls. The compiler may ignore it.

**`@[opaque]`.** A struct field marked `@[opaque]` cannot be initialized, read, or assigned from Lucid source. It is invisible to `toStr` and to `satisfy` field checks. A struct with only `@[opaque]` fields has no literal form.

**`@[host_only]`.** The declaration can only be called from C++. Lucid code cannot call it. This is used for functions that the host uses as callbacks or that the host provides as primitives.

### Placement

Attributes appear before the declaration they modify:

```lucid
@[export]
const add fn (a int, b int) -> int = { return a + b; };

@[inline]
const square fn (x int) -> int = { return x * x; };

struct Config {
    @[deprecated("use maxConnections instead")]
    max_conn int = 100;
    maxConnections int = 100;
}
```

Attributes on local declarations are permitted, except for `@[export]`.

### The attribute set is closed

The attributes listed above are the only ones the language recognizes. An unknown attribute is a parse error:

```
error: unknown attribute 'unknown'
   --> main.luc:1:3
    |
  1 | @[unknown]
    |   ^^^^^^^
    |
   = help: valid attributes are: export, foreign, link, deprecated,
     inline, noinline, opaque, host_only
```

There is no user-defined attribute form. If a future need arises for a compile-time extensibility mechanism, it will be a macro system, not an opening of the attribute namespace.

---

## The `#builtin` Registry

The `#builtin` registry is the set of compiler-recognized operations. A `#builtin` handler is not a runtime function; it is an operation the compiler emits directly.

### What `#builtin` is

A `#builtin` handler differs from `#host` and `#native`:

- **`#host`** binds to a C++ function registered with the engine. The call is a real function call.
- **`#native`** binds to a VM opcode. The call is a VM instruction.
- **`#builtin`** is a compiler-emitted operation. The "call" is not a function call at all; the compiler emits the operation inline.

A `#builtin` is used for operations that reflect on types, manipulate compiler-level entities, or require the compiler to emit specialized code per type.

### The registry

Each entry has a name (the string in `#builtin(name)`), a signature, and a description of what the compiler does with it.

**Type introspection:**

| Name           | Signature         | Behavior                                 |
| -------------- | ----------------- | ---------------------------------------- |
| `size_of`      | `<T>() -> uint64` | Byte size of `T`                         |
| `align_of`     | `<T>() -> uint64` | ABI alignment of `T`                     |
| `type_name`    | `<T>() -> string` | Concrete type name at each instantiation |
| `field_count`  | `<T>() -> uint64` | Number of fields in a struct `T`         |
| `is_enum`      | `<T>() -> bool`   | Compile-time predicate                   |
| `is_struct`    | `<T>() -> bool`   | Compile-time predicate                   |
| `is_primitive` | `<T>() -> bool`   | Compile-time predicate                   |

**Memory:**

| Name      | Signature                                | Behavior                              |
| --------- | ---------------------------------------- | ------------------------------------- |
| `alloc`   | `<T>(count uint64) -> &T`                | Allocate `count` values of `T`        |
| `free`    | `<T>(p &T)`                              | Free a previously allocated reference |
| `memcpy`  | `<T>(dst &T, src &T, bytes uint64)`      | Copy bytes                            |
| `memmove` | `<T>(dst &T, src &T, bytes uint64)`      | Copy bytes, overlap-safe              |
| `memset`  | `<T>(dst &T, value uint8, bytes uint64)` | Fill bytes                            |
| `realloc` | `<T>(p &T, newCount uint64) -> &T`       | Reallocate                            |

**Strings:**

| Name             | Signature                                  | Behavior                 |
| ---------------- | ------------------------------------------ | ------------------------ |
| `int_to_str`     | `(v int) -> string`                        | Decimal signed integer   |
| `uint_to_str`    | `(v uint) -> string`                       | Decimal unsigned integer |
| `float_to_str`   | `(v float) -> string`                      | Decimal float            |
| `bool_to_str`    | `(v bool) -> string`                       | `"true"` or `"false"`    |
| `char_to_str`    | `(v char) -> string`                       | Single character         |
| `ptr_to_hex_str` | `<T>(p &T) -> string`                      | Address as hex string    |
| `str_concat`     | `(a string, b string) -> string`           | Concatenation            |
| `str_len`        | `(s string) -> uint`                       | Byte length              |
| `str_eq`         | `(a string, b string) -> bool`             | Byte equality            |
| `str_lt`         | `(a string, b string) -> bool`             | Byte less-than           |
| `str_slice`      | `(s string, from uint, to uint) -> string` | Byte slice               |
| `str_from_ptr`   | `(p &uint8, len uint) -> string`           | Construct from bytes     |

**Struct and enum:**

| Name            | Signature                              | Behavior               |
| --------------- | -------------------------------------- | ---------------------- |
| `struct_to_str` | `<T : StructType>(v T) -> string`      | Per-field `toStr` walk |
| `enum_to_str`   | `<T : EnumType>(v T) -> string`        | Variant name           |
| `enum_tag`      | `<T : EnumType>(v T) -> int`           | Discriminant           |
| `enum_is`       | `<T : EnumType>(v T, tag int) -> bool` | Discriminant check     |

**Closures:**

| Name        | Signature                               | Behavior                         |
| ----------- | --------------------------------------- | -------------------------------- |
| `fn_to_cls` | `<A, B>(f fn (A) -> B) -> cls (A) -> B` | Wrap bare function with null env |
| `call_fn`   | `<A, B>(f fn (A) -> B, a A) -> B`       | Call a bare function             |
| `call_cls`  | `<A, B>(f cls (A) -> B, a A) -> B`      | Call a closure                   |

**References and weak:**

| Name           | Signature                | Behavior                 |
| -------------- | ------------------------ | ------------------------ |
| `weak`         | `<T>(v &T) -> Weak<T>`   | Construct weak reference |
| `upgrade`      | `<T>(w Weak<T>) -> &T?`  | Strong ref if alive      |
| `strong_count` | `<T>(v &T) -> uint`      | Strong reference count   |
| `weak_count`   | `<T>(w Weak<T>) -> uint` | Weak reference count     |

**Fibers:**

| Name              | Signature                          | Behavior                           |
| ----------------- | ---------------------------------- | ---------------------------------- |
| `spawn_fiber`     | `<T>(f fn () -> T)`                | Run `f` on a fiber, discard result |
| `start_fiber`     | `<T>(f fn () -> T) -> Deferred<T>` | Run `f` on a fiber, return handle  |
| `await_deferred`  | `<T>(d &Deferred<T>) -> T`         | Suspend until resolved             |
| `cancel_deferred` | `<T>(d &Deferred<T>)`              | Request cancellation               |
| `deferred_ready`  | `<T>(d &Deferred<T>) -> bool`      | Whether resolved                   |

**Errors:**

| Name        | Signature      | Behavior                                 |
| ----------- | -------------- | ---------------------------------------- |
| `panic_str` | `(msg string)` | Print message and stack trace, terminate |
| `warn_str`  | `(msg string)` | Print to warning channel                 |

**Maps:**

| Name         | Signature                           | Behavior           |
| ------------ | ----------------------------------- | ------------------ |
| `map_new`    | `<K, V>() -> Map<K, V>`             | Create a map       |
| `map_len`    | `<K, V>(m &Map<K, V>) -> uint`      | Entry count        |
| `map_get`    | `<K, V>(m &Map<K, V>, k K) -> V?`   | Lookup             |
| `map_set`    | `<K, V>(m &Map<K, V>, k K, v V)`    | Insert or replace  |
| `map_has`    | `<K, V>(m &Map<K, V>, k K) -> bool` | Key present        |
| `map_remove` | `<K, V>(m &Map<K, V>, k K) -> bool` | Remove if present  |
| `map_keys`   | `<K, V>(m &Map<K, V>) -> [*]K`      | Snapshot of keys   |
| `map_values` | `<K, V>(m &Map<K, V>) -> [*]V`      | Snapshot of values |

**Arrays:**

| Name           | Signature                   | Behavior        |
| -------------- | --------------------------- | --------------- |
| `array_len`    | `<T>(a [_]T) -> uint`       | Length          |
| `array_push`   | `<T>(a &[*]T, v T)`         | Append          |
| `array_pop`    | `<T>(a &[*]T) -> T?`        | Remove last     |
| `array_insert` | `<T>(a &[*]T, i uint, v T)` | Insert at index |
| `array_remove` | `<T>(a &[*]T, i uint)`      | Remove at index |
| `array_resize` | `<T>(a &[*]T, n uint)`      | Resize          |
| `array_clear`  | `<T>(a &[*]T)`              | Clear           |

**Op-kind registration:**

| Name               | Signature                 | Behavior            |
| ------------------ | ------------------------- | ------------------- |
| `register_op_kind` | `(name string) -> OpKind` | Register an op kind |

### What is not in the registry

- **User-facing functions** (`println`, `print`, `toStr`, `error`, `warn`). These are defined in terms of the builtins above and declared in the core script.
- **Operators** (`+`, `-`, `==`). These are `DEF`s backed by `#native` handlers, not `#builtin`.
- **Standard library algorithms** (`array_map`, `array_filter`, etc.). These are ordinary Lucid functions.

### Stability

The `#builtin` registry is the compiler's hardcoded surface. It changes only when a new operation requires compiler-emitted code. The list is short; adding to it is a compiler change.

### `#builtin` vs `#native`

A `#native` handler is a VM opcode. It exists in the VM's instruction set and is available to any function that binds to it.

A `#builtin` handler is a compiler-emitted operation. It does not exist as a function; the compiler generates specialized code for it, often per-type or per-context.

The distinction:

- `#native` — the operation is a fixed VM instruction.
- `#builtin` — the operation's code depends on the surrounding context (usually the type arguments).

`array_push<T>` is `#builtin` because the code depends on `T`'s size and layout. `add_i32` is `#native` because the operation is one fixed instruction.

---

## The `#hostFn` Boundary

A host function may be passed to `host_async` as a `#hostFn`:

```lucid
FN host_async<T> (work #hostFn) -> Deferred<T> = #host(host_run_async);
```

`#hostFn` is a special type for host-only functions that can be scheduled on a worker thread. It is not a normal function type; it is recognized by the `host_async` builtin.

```lucid
@[host_only]
const computePhysics fn (worldId int, dt float) -> PhysicsResult = {};

const async step fn (worldId int, dt float) -> PhysicsResult = {
    let d Deferred<PhysicsResult> = host_async(computePhysics(worldId, dt));
    return await d;
};
```

The `@[host_only]` attribute marks the function as callable only from C++. Passing it to `host_async` is the one way to schedule it from Lucid.

---

## Interop Summary

The interop surface:

| Boundary                       | Mechanism                          | Direction        |
| ------------------------------ | ---------------------------------- | ---------------- |
| C functions into Lucid         | `@[foreign("C")]` + `@[link(...)]` | C → Lucid        |
| Lucid functions out to C       | `@[export, foreign("C")]`          | Lucid → C        |
| C++ classes into Lucid         | `extern "C"` wrappers              | C++ → Lucid      |
| Host functions into Lucid      | `#host(...)`                       | Host → Lucid     |
| VM primitives into Lucid       | `#native(...)`                     | VM → Lucid       |
| Compiler operations into Lucid | `#builtin(...)`                    | Compiler → Lucid |
| Engine parallelism from Lucid  | `host_async` + `#hostFn`           | Lucid → Host     |

The memory model:

- Lucid's refcounting is internal. `&T` references are managed; C sees raw pointers.
- C's memory is not managed by Lucid. It is freed by C.
- The boundary is explicit: pointers passed to C are raw; pointers received from C are converted with `strFromPtr` or wrapped in Lucid-owned storage.

---

## Operator Precedence

The precedence table, highest binding to lowest. Operators on the same level associate as noted.

| Level | Operators                   | Associativity | Notes                                            |
| ----- | --------------------------- | ------------- | ------------------------------------------------ |
| 1     | postfix `()` `[]` `.` `::`  | left          | calls, indexing, field access, module access     |
| 2     | unary `-` `not` `~` `&`     | right         | negation, logical not, bitwise not, reference-of |
| 3     | `**`                        | right         | exponentiation                                   |
| 4     | `*` `/` `%`                 | left          | multiplicative                                   |
| 5     | `+` `-`                     | left          | additive                                         |
| 6     | `..` `..<`                  | left          | range                                            |
| 7     | `==` `!=` `<` `<=` `>` `>=` | left          | comparison                                       |
| 8     | `and`                       | left          | logical and (short-circuit)                      |
| 9     | `or`                        | left          | logical or (short-circuit)                       |
| 10    | `??`                        | left          | null coalescing                                  |
| 11    | `\|>`                       | left          | pipeline                                         |

### Binding rules

**Level 1 — postfix.** The postfix operators bind tightest. `f(x)[0].field` parses as `(((f(x))[0]).field)`. The postfix operators are:

- `()` — call.
- `[]` — index.
- `.` — field access.
- `::` — module or static member access.

**Level 2 — unary.** Unary operators bind right, so `- -x` parses as `-(-x)`. The unary operators are:

- `-` — arithmetic negation.
- `not` — logical not.
- `~` — bitwise not.
- `&` — reference-of (in expression position; in type position `&` is a type marker).

**Level 3 — `**`.** Right-associative, so `2 ** 3 ** 2` parses as `2 ** (3 ** 2)`.

**Level 4 — `*` `/` `%`.** Left-associative. `a * b / c` parses as `(a * b) / c`.

**Level 5 — `+` `-`.** Left-associative. `a + b - c` parses as `(a + b) - c`.

**Level 6 — `..` `..<`.** Left-associative. A range binds tighter than comparison, so `0..n - 1` parses as `0..(n - 1)` — the arithmetic is evaluated first. It binds looser than additive, so `a..b + c` parses as `a..(b + c)`.

**Level 7 — comparison.** Left-associative. `a < b == c` parses as `(a < b) == c`. Chained comparisons are not supported; `a < b < c` parses as `(a < b) < c`, which is a type error (`bool < c`).

**Level 8 — `and`.** Left-associative, short-circuit. `a and b and c` parses as `(a and b) and c`, but evaluates left-to-right and short-circuits.

**Level 9 — `or`.** Left-associative, short-circuit. Binds looser than `and`.

**Level 10 — `??`.** Left-associative. Binds looser than `or`, so `a ?? b or c` parses as `a ?? (b or c)`. Binds tighter than `|>`.

**Level 11 — `|>`.** Left-associative. Binds loosest; a pipeline extends as far right as possible.

### Precedence examples

```lucid
1 + 2 * 3                  -- 1 + (2 * 3) = 7
(1 + 2) * 3                -- 9
2 ** 3 ** 2                -- 2 ** (3 ** 2) = 512
- -x                       -- -(-x) = x
not a and b                -- (not a) and b
a or b and c               -- a or (b and c)
a ?? b or c                -- a ?? (b or c)
x |> f |> g                -- (x |> f) |> g
0..n - 1                   -- 0..(n - 1)
a < b == c                 -- (a < b) == c
```

### `&` in type position vs expression position

In type position, `&` is a reference marker: `&T`.

In expression position, `&` is the bitwise AND operator:

```lucid
let a int = 0xFF & 0x0F;
```

The parser distinguishes by position: a type position is a `type` production; an expression position is an `expr` production. The two do not overlap, and the same token means different things.

### `??` and `|>` in the pipeline

`??` binds tighter than `|>`, so `x ?? y |> f` parses as `(x ?? y) |> f`. The fallback is evaluated first, then piped.

`|>` binds loosest, so `a |> f(b)! |> g` parses as `(a |> f(b)!) |> g`.

### Parenthesization

Parentheses override precedence:

```lucid
(1 + 2) * 3
a and (b or c)
(x |> f) |> g
```

### Operator tokens

Every operator token in the precedence table is a `DEF`-backed operation. What `+` *means* for a specific pair of types is determined by the `DEF` table. The precedence table determines how operators are grouped syntactically; the `DEF` table determines their semantics.

---

## The Boot Set

The boot set is the fixed vocabulary the parser recognizes before any script loads. It has six parts.

### Frame keywords

**Lowercase (value frame):**

```
const   let   import   trait   satisfy
```

**Uppercase (host and behavior frames):**

```
TYPE   FN   DEF   REQUIRE
```

### Content markers

```
struct   enum   fn   cls   as   Self
```

Content markers appear inside a frame's target position. `struct` and `enum` may also start a top-level declaration as sugar for `TYPE X = struct { ... }` / `TYPE X = enum { ... }`.

### Statement keywords

```
if   else   for   while   do   switch   case   default
break   continue   return
async   spawn   start   await   all   any
```

`all` and `any` are only valid immediately after `await`. `async` appears in a declaration's header.

### Punctuation

**Delimiters:**

```
(   )   {   }   [   ]
```

**Separators:**

```
,   ;   ::   .   ..
```

**Operators and markers:**

```
=   ->   ?   !
+   -   *   /   %   **
==   !=   <   <=   >   >=
and   or   not
&   |   ^   ~   <<   >>
|>   ??
```

### Literals

```
nil   err   true   false
```

Plus the numeric, string, and char literal forms.

`nil` and `err` are boot literals. Their types (`Option<T>` and `Fallible<T>`) are declared in the core script, but the tokens themselves are recognized by the lexer.

### The boot type `Type`

`Type` is the type of types. It is the parameter type of `#builtin` handlers that reflect on type structure. It is not user-visible as a name.

### Attribute names

```
export   foreign   link   deprecated   inline   noinline   opaque   host_only
```

The parser recognizes these names because it must reject unknown attributes at parse time.

### What is not in the boot set

Everything else is declared in a core script:

- Every primitive type name (`int`, `float`, `bool`, `string`, `char`, etc.).
- Every type declared by a user or by the core scripts (`Vec2`, `Map`, `Deferred`, `Weak`, `Option`, `Fallible`, etc.).
- Every operator's meaning (`+`, `-`, `==`, etc.) — the symbols are boot punctuation; their `DEF`s are core-script declarations.
- Every function (`println`, `toStr`, `sizeof`, `alloc`, `error`, `warn`, `weak`, `upgrade`, etc.).
- Every trait (`Eq`, `Ord`, `Numeric`, `Stringable`, etc.).
- Every `OpKind` (`BINARY_OP`, `UNARY_OP`, `INDEX_GET`, `INDEX_SET`, `CALL`).
- The `#host`, `#native`, `#builtin` markers — recognized as target kinds by the parser, but their resolution is the compiler's.
- The `#builtin` registry names — the compiler's hardcoded operations.

### Boot set stability

The boot set is the language's fixed vocabulary. Adding or removing a keyword is a breaking change. The boot set should only change across major versions of the language.

The `#builtin` registry is a companion stability boundary; it changes only when a new compiler-emitted operation is needed.

---

## Open Items

Decisions deliberately left for later, with the reasons and the current stance.

### Cycle detector (Godot-style `collect_cycles()`)

**Status:** deferred.

A function that walks the object graph, marks reachable values, and clears unreachable cycles is not in the initial design. Adding it requires a global object registry, accurate root discovery, and a defined safe point. It is an additive VM-level feature, not a language change.

**Current stance:** refcounting with `Weak<T>` is the mechanism. Cycles leak unless broken. The compiler warns on obvious same-scope cycles.

### Loop labels

**Status:** deferred.

Lucid has no labeled loops. To break out of an outer loop, use a flag. Labeled loops would be a small addition; they are not in this version.

### General pattern matching

**Status:** deferred.

`switch` matches values, enum variants, payload bindings, and literal ranges. `if let` narrows a nullable or fallible value. A more general pattern-matching form — binding multiple values, destructuring tuples, matching against structural patterns — is not in this version.

### Structural conformance (auto-satisfy)

**Status:** rejected for now.

A type does not automatically satisfy a trait by having the required fields; a `satisfy` block must assert it. This is consistent with the design's explicit stance, but a future version could allow field-only traits to be satisfied structurally without a `satisfy` block. The cost is that a type could satisfy a trait "by accident."

### Macro system

**Status:** deferred.

There is no macro system. Attributes are a closed set. If compile-time extensibility is needed, it will be a macro system, not an opening of the attribute namespace.

### User-defined attributes

**Status:** rejected.

Attributes are a closed set of eight names. There is no user-defined attribute form. Typos in attribute names are parse errors.

### `&Trait` (trait objects)

**Status:** rejected.

A `&Trait` — a reference to a value whose concrete type is erased down to a trait's shape — is not supported and is not planned. It would require vtable-style dispatch, a new type kind, and runtime machinery. The recommended alternative for "several concrete types unified by shared fields" is a closed enum with payloads.

### Tuple types

**Status:** rejected.

Lucid has no tuple type. Grouping several values is done with a generic struct:

```lucid
struct Pair<A, B> { first A; second B; }
```

### Named arguments

**Status:** rejected.

Function calls are positional. There is no named-argument syntax.

### Default parameter values

**Status:** rejected.

A function's parameters have no defaults. Defaults are a struct field's feature, not a function's. To simulate a default parameter, write an overload-like set of helper functions with distinct names, or use a struct to bundle optional arguments.

### Variadic `println`

**Status:** rejected.

`println` takes exactly one argument. To print several values, concatenate with `++` and `toStr`, or use string interpolation.

### Map literal syntax

**Status:** rejected.

There is no map literal. `Map<K, V>` is constructed with `map_new` and populated with `INDEX_SET`. This follows from the general rule that a struct with only `@[opaque]` fields has no literal form.

A `map_of` helper is provided in `core.map` for the "build from a list of pairs" case.

### `Array<T>` wrapper

**Status:** rejected.

Arrays are the boot-level types `[*]T`, `[_]T`, `[N]T`. There is no `Array<T>` type. Array operations are free functions in `core.array`.

### Method syntax

**Status:** rejected.

There is no method-call syntax. `v.method()` is not valid; behavior lives in free functions. Static struct members are accessed with `::` (`Vec2::zero()`); instance fields with `.` (`v.x`).

### Operator overloading beyond `DEF`

**Status:** rejected.

Operators are overloaded through `DEF` declarations, keyed by operand types. There is no other overloading mechanism. Function names are not overloadable.

### Type inference

**Status:** rejected.

Every type is written. There is no inference from initializers or from context. Loop variables, parameters, return types, and bindings all carry explicit types.

### Implicit conversions

**Status:** rejected (except `fn → cls`).

The `fn → cls` coercion is the only implicit conversion. There is no implicit numeric promotion, no implicit reference upcast, no implicit string conversion.

### `T!?` order

**Status:** rejected.

`?!` is the only valid order for the combined nullable-fallible type. `!?` is a parse error.

### Nullable arrays

**Status:** rejected.

`?` and `!` bind to the element type. `[*]int?` is an array of nullable ints. A nullable array does not exist; use an empty array to signal "no array."

### Nullable function types

**Status:** rejected.

`?` and `!` do not apply to function types. A nullable function type does not exist.

---

## Summary of the Language

A one-page summary of the entire grammar.

### Types

- **Primitives** — declared in the core script: `int`, `float`, `bool`, `string`, `char`, `byte`, and their sized variants.
- **Structs** — `struct X { ... }` or `TYPE X = struct { ... }`. Fields with optional defaults. `@[opaque]` for host-only fields.
- **Enums** — `enum X { ... }` or `TYPE X = enum { ... }`. Integer variants and payload variants.
- **Generics** — `<T>`, `<T : Trait>`, `<T : A + B>`. Specialization only; no erasure.
- **Functions** — `fn` (bare pointer) and `cls` (closure). Per-stage markers.
- **Arrays** — `[*]T` (dynamic), `[_]T` (slice), `[N]T` (fixed).
- **References** — `&T` (strong, refcounted), `Weak<T>` (weak).
- **Nullable, fallible, combined** — `T?`, `T!`, `T?!`. Lowered to `Option<T>`, `Fallible<T>`, `Both<T>`.

### Declarations

- **`TYPE`** — declare a named type. Targets: `#host`, `#native`, `#builtin`, an alias, a `struct` body, an `enum` body.
- **`FN`, `const`, `let`** — declare a value or callable. `FN` for host-backed; `const` immutable; `let` mutable.
- **`trait`** — declare a trait. Clauses: `FIELD` (field requirements) and `REQUIRE` (operation requirements).
- **`satisfy`** — assert a type satisfies a trait.
- **`DEF`** — declare an operator or named-call fact.
- **`import`** — bring a module into scope.

### Statements

- **Declarations** — `const`, `let`, `FN`, `TYPE`, `trait`, `satisfy`, `DEF`.
- **Assignments** — `x = value`, `x += 1`, and the other compound forms.
- **Control flow** — `if`, `else`, `for`, `while`, `do`/`while`, `switch`.
- **Jumps** — `return`, `break`, `continue`.
- **Concurrency** — `spawn`, `start`, `await`.

### Expressions

- **Literals** — numbers, strings, chars, booleans, `nil`, `err`.
- **Access** — identifiers, field access, indexing, slicing, module access.
- **Calls** — function calls, curried calls, calls through function-typed values.
- **Operators** — binary, unary, assignment, all resolved through `DEF`.
- **Pipeline** — `|>` with optional `!` argument pack.
- **Fallback** — `??`.
- **Function literals** — anonymous functions with inferred shape.
- **Control flow expressions** — `if cond ?? then else else`.

### Concurrency

- One VM thread; cooperative fibers.
- `async` marks a function as a fiber entry point.
- `spawn f(args)` — fire and forget.
- `start d T = f(args)` — held handle; `Deferred<T>`.
- `await d` — consume; narrows to `T`.
- `Deferred<T>` is a linear value: consumed exactly once; cannot be stored in fields or arrays; cannot be captured.
- `cancel(d)` requests cancellation.
- True parallelism lives on the C++ side, reached through `host_async`.

### Memory

- `T` copies; `&T` aliases; the referent is refcounted.
- `Weak<T>` breaks cycles.
- Cycles of strong references leak unless broken.
- `@[opaque]` fields are host-only.
- `Arena` (if present) is scope-confined.

### Interop

- `@[foreign("C")]` for C functions.
- `@[link(...)]` for library and file paths.
- C++ through `extern "C"` wrappers.
- `#host` for host-registered functions.
- `#native` for VM opcodes.
- `#builtin` for compiler-emitted operations.
- `host_async` for engine parallelism.

### Standard library

- `core` — primitives, operators, `Option`, `Fallible`, `Both`, `Deferred`, `Weak`, `toStr`, `print`, `error`, `warn`.
- `core.map` — `Map<K, V>`.
- `core.array` — array operations and functional helpers.
- `core.string` — string manipulation.
- `core.math` — arithmetic utilities.
- `core.io` — console and file I/O.
- `core.fn` — function composition.

## Quick Reference Tables

These tables collect names already defined throughout this document into one lookup point, grouped by kind. Every entry links back to where it is actually specified — nothing here is a new decision, only a consolidation.

### Keywords

| Category             | Keywords                                                                             | Notes                                                                                                                  |
| -------------------- | ------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------- |
| Type/behavior frames | `TYPE` `FN` `DEF` `REQUIRE`                                                          | Uppercase — see [The Type Frame — `TYPE`](#the-type-frame-type), [The Behavior Frame — `DEF`](#the-behavior-frame-def) |
| Value frames         | `const` `let`                                                                        | `const` binds immutably, `let` mutably — see [The Value Frame — `FN`, `const`, `let`](#the-value-frame-fn-const-let)   |
| Module/trait frames  | `import` `trait` `satisfy`                                                           | See [The Trait Frame — `trait`, `satisfy`, `REQUIRE`](#the-trait-frame-trait-satisfy-require)                          |
| Type content         | `struct` `enum` `fn` `cls` `as` `Self`                                               | `fn`/`cls` mark a function type's calling convention; `Self` is the trait-body type placeholder                        |
| Statements           | `if` `else` `for` `while` `do` `switch` `case` `default` `break` `continue` `return` | See [Statements](#statements)                                                                                          |
| Concurrency          | `async` `spawn` `start` `await` `all` `any`                                          | See [`async`, `spawn`, `start`, `await`](#async-spawn-start-await)                                                     |
| Literals             | `nil` `err` `true` `false`                                                           | `nil` — [Nullable, Fallible, and Combined Types](#nullable-fallible-and-combined-types); `err` — same chapter, `T!`    |

The keyword set is closed. No keyword may be used as an identifier.

### Attributes

| Attribute              | Valid on               | Meaning                                |
| ---------------------- | ---------------------- | -------------------------------------- |
| `@[export]`            | top-level declarations | Visible outside the file               |
| `@[foreign("C")]`      | function declarations  | Implemented in a foreign ABI           |
| `@[link("name", ...)]` | declarations           | Link against native libraries or files |
| `@[deprecated("msg")]` | any declaration        | Compiler warning at use sites          |
| `@[inline]`            | function declarations  | Inlining hint                          |
| `@[noinline]`          | function declarations  | Prevent inlining                       |
| `@[opaque]`            | struct fields          | No Lucid-side access                   |
| `@[host_only]`         | declarations           | Callable only from C++                 |

The attribute set is closed — an unknown attribute is a parse error. Full semantics: [Attributes](#attributes).

### Target Sigils

| Sigil            | Meaning                                                        | Used in                                        |
| ---------------- | -------------------------------------------------------------- | ---------------------------------------------- |
| `@[...]`         | Metadata about a declaration that already has its own identity | Attributes, above                              |
| `#host(name)`    | Implementation is a C++ function registered with the engine    | `TYPE`, `FN`, `DEF` targets                    |
| `#native(name)`  | Implementation is a VM opcode (core scripts only)              | `TYPE`, `FN`, `DEF` targets                    |
| `#builtin(name)` | Implementation is a compiler-emitted operation                 | `TYPE`, `FN`, `DEF` targets                    |
| `#hostFn`        | Marks a function value as schedulable on a worker thread       | [The `#hostFn` Boundary](#the-hostfn-boundary) |

### Built-in Types

| Type                | Backing                | Notes                                                                           |
| ------------------- | ---------------------- | ------------------------------------------------------------------------------- |
| `bool`              | `#host(bool)`          |                                                                                 |
| `char`              | `#host(char)`          |                                                                                 |
| `string`            | `#host(string)`        |                                                                                 |
| `byte` / `int8`     | `#host(int8)`          | `int8` is an alias of `byte`                                                    |
| `short` / `int16`   | `#host(int16)`         | `int16` is an alias of `short`                                                  |
| `int` / `int32`     | `#host(int32)`         | `int32` is an alias of `int`; the default integer type                          |
| `long` / `int64`    | `#host(int64)`         | `int64` is an alias of `long`                                                   |
| `ubyte` / `uint8`   | `#host(uint8)`         | `uint8` is an alias of `ubyte`                                                  |
| `ushort` / `uint16` | `#host(uint16)`        | `uint16` is an alias of `ushort`                                                |
| `uint` / `uint32`   | `#host(uint32)`        | `uint32` is an alias of `uint`                                                  |
| `ulong` / `uint64`  | `#host(uint64)`        | `uint64` is an alias of `ulong`                                                 |
| `float`             | `#host(float)`         | 32-bit; the default floating type                                               |
| `double`            | `#host(double)`        | 64-bit                                                                          |
| `unit`              | `#host(unit)`          | The empty/no-value return type                                                  |
| `Map<K, V>`         | `#host(LucidMap)`      | See [`Map<K, V>`](#mapk-v)                                                      |
| `Deferred<T>`       | `#host(LucidDeferred)` | See [`Deferred<T>` and Concurrency Support](#deferredt-and-concurrency-support) |
| `Weak<T>`           | `#host(LucidWeak)`     | See [`Weak<T>` and Cycle Handling](#weakt-and-cycle-handling)                   |
| `Option<T>`         | `#builtin`             | See [`Option<T>`, `Fallible<T>`, `Both<T>`](#optiont-falliblet-botht)           |
| `Fallible<T>`       | `#builtin`             | Same chapter as above                                                           |
| `Both<T>`           | `#builtin`             | Same chapter as above                                                           |
| `OpKind`            | `#host(OpKind)`        | See [The Standard `OpKind` Set](#the-standard-opkind-set)                       |

The primitive set is a starting catalog, not closed — a core script may declare more with `TYPE X = #host(...)`.

### Built-in Traits

| Trait                | Requires                 | Composed from                                        |
| -------------------- | ------------------------ | ---------------------------------------------------- |
| `Eq`                 | `BINARY_OP '=='`         | —                                                    |
| `Ord`                | `BINARY_OP '<'`          | `Eq`                                                 |
| `Add`                | `BINARY_OP '+'`          | —                                                    |
| `Sub`                | `BINARY_OP '-'`          | —                                                    |
| `Mul`                | `BINARY_OP '*'`          | —                                                    |
| `Div`                | `BINARY_OP '/'`          | —                                                    |
| `Rem`                | `BINARY_OP '%'`          | —                                                    |
| `Neg`                | `UNARY_OP '-'`           | —                                                    |
| `Numeric`            | (nothing new)            | `Add`, `Sub`, `Mul`, `Div`, `Neg`                    |
| `Integral`           | (nothing new)            | `Numeric`, `Rem`, `Eq`, `Ord`                        |
| `Stringable`         | `CALL 'toStr'`           | —                                                    |
| `StructType`         | (marker, no clauses)     | Auto-`satisfy`d by every struct                      |
| `EnumType`           | (marker, no clauses)     | Auto-`satisfy`d by every enum                        |
| `Indexable<K, V>`    | `INDEX_GET`              | —                                                    |
| `IndexableMut<K, V>` | `INDEX_GET`, `INDEX_SET` | `Indexable<K, V>` in spirit (both required directly) |

Full definitions: [The Trait Catalog](#the-trait-catalog).

### Operators (`DEF` `OpKind`s)

| `OpKind`    | Symbol slot                                                                            | Signature shape                     | Example                                                       |
| ----------- | -------------------------------------------------------------------------------------- | ----------------------------------- | ------------------------------------------------------------- |
| `BINARY_OP` | `+` `-` `*` `/` `%` `**` `==` `!=` `<` `<=` `>` `>=` `&` `\|` `^` `<<` `>>` `and` `or` | `(a T, b T) -> T`                   | `DEF BINARY_OP '+' (a int, b int) -> int = #native(add_i32);` |
| `UNARY_OP`  | `-` `not` `~`                                                                          | `(v T) -> T`                        | `DEF UNARY_OP '-' (v int) -> int = #native(neg_i32);`         |
| `INDEX_GET` | (none)                                                                                 | `(container, index) -> value`       | `DEF INDEX_GET (a [*]T, i uint) -> T = #builtin(array_get);`  |
| `INDEX_SET` | (none)                                                                                 | `(container, index, value) -> unit` | `DEF INDEX_SET (a &[*]T, i uint, v T) = #builtin(array_set);` |
| `CALL`      | any name, e.g. `'toStr'`                                                               | `(receiver, ...args) -> value`      | `DEF CALL 'toStr' (v int) -> string = #builtin(int_to_str);`  |

Full mechanics: [The Standard `OpKind` Set](#the-standard-opkind-set), [Overload Resolution](#overload-resolution), [Operator Precedence](#operator-precedence).

### Built-in Functions

**I/O and diagnostics** (`core`, `core.io`)

| Function  | Signature      | Notes                                                                |
| --------- | -------------- | -------------------------------------------------------------------- |
| `write`   | `(s string)`   | `#host(host_write)` — the raw output primitive                       |
| `print`   | `<T> (v T)`    | Calls `toStr`, no trailing newline                                   |
| `println` | `<T> (v T)`    | Calls `toStr`, trailing newline                                      |
| `error`   | `(msg string)` | Panics the current fiber — see [`error` and `warn`](#error-and-warn) |
| `warn`    | `(msg string)` | Prints to the warning channel, does not unwind                       |

**Array** (`core.array`)

| Function       | Signature                    | Notes |
| -------------- | ---------------------------- | ----- |
| `array_len`    | `<T> (a [_]T) -> uint`       |       |
| `array_push`   | `<T> (a &[*]T, v T)`         |       |
| `array_pop`    | `<T> (a &[*]T) -> T?`        |       |
| `array_insert` | `<T> (a &[*]T, i uint, v T)` |       |
| `array_remove` | `<T> (a &[*]T, i uint)`      |       |
| `array_resize` | `<T> (a &[*]T, n uint)`      |       |
| `array_clear`  | `<T> (a &[*]T)`              |       |

**String** (`core.string`)

| Function     | Signature                                  | Notes                           |
| ------------ | ------------------------------------------ | ------------------------------- |
| `strLen`     | `(s string) -> uint`                       |                                 |
| `strEq`      | `(a string, b string) -> bool`             |                                 |
| `strConcat`  | `(a string, b string) -> string`           | Same as `++` on two strings     |
| `strSlice`   | `(s string, from uint, to uint) -> string` |                                 |
| `strFromPtr` | `(p &uint8, len uint) -> string`           | Host-boundary construction only |

**Math** (`core.math`)

| Function                                                    | Signature                                      | Notes                            |
| ----------------------------------------------------------- | ---------------------------------------------- | -------------------------------- |
| `sqrt` `pow` `sin` `cos` `tan` `floor` `ceil` `round` `abs` | `(x float) -> float` (`pow` takes `base, exp`) | All `#native`, direct VM opcodes |
| `random`                                                    | `() -> float`                                  |                                  |
| `randomInt`                                                 | `(lo int, hi int) -> int`                      |                                  |
| `seedRandom`                                                | `(seed uint64)`                                |                                  |

**Map** (`core.map`)

| Function     | Signature                            | Notes |
| ------------ | ------------------------------------ | ----- |
| `map_new`    | `<K, V> () -> Map<K, V>`             |       |
| `map_len`    | `<K, V> (m &Map<K, V>) -> uint`      |       |
| `map_has`    | `<K, V> (m &Map<K, V>, k K) -> bool` |       |
| `map_remove` | `<K, V> (m &Map<K, V>, k K) -> bool` |       |
| `map_keys`   | `<K, V> (m &Map<K, V>) -> [*]K`      |       |
| `map_values` | `<K, V> (m &Map<K, V>) -> [*]V`      |       |

**`Weak<T>`** (`core`)

| Function      | Signature                 | Notes                         |
| ------------- | ------------------------- | ----------------------------- |
| `weak`        | `<T> (v &T) -> Weak<T>`   |                               |
| `upgrade`     | `<T> (w Weak<T>) -> &T?`  | Fails if the referent is gone |
| `strongCount` | `<T> (v &T) -> uint`      |                               |
| `weakCount`   | `<T> (w Weak<T>) -> uint` |                               |

**`Deferred<T>` and concurrency support** (`core`)

| Function     | Signature                           | Notes                                         |
| ------------ | ----------------------------------- | --------------------------------------------- |
| `isReady`    | `<T> (d &Deferred<T>) -> bool`      | Never required for correctness — polling only |
| `cancel`     | `<T> (d &Deferred<T>)`              |                                               |
| `host_async` | `<T> (work #hostFn) -> Deferred<T>` | The bridge to engine-side parallelism         |

Full listings and any functions added since this table was built: [The Core Scripts](#the-core-scripts) onward, Part VIII.