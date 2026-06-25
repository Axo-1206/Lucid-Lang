# Luc AST Node Reference

This document maps the Luc grammar (see `/docs/LUC_GRAMMAR.md`) to the concrete AST node definitions in `BaseAST.hpp`, `DeclAST.hpp`, `ExprAST.hpp`, `StmtAST.hpp`, and `TypeAST.hpp`. It explains how each syntactic construct is represented and highlights design decisions, potential gaps, and suggested improvements.

> **Grammar reference**: Each section links to the relevant grammar rule in `/docs/LUC_GRAMMAR.md`. For the exact syntax, consult that file.

---

## 1. Base Infrastructure

### `BaseAST`
- **Grammar**: (implicit root of all nodes)
- **Fields**: `kind` (ASTKind enum), `loc` (SourceLocation)
- **Purpose**: Root of all AST nodes. Provides `isa<T>()` and `as<T>()` helpers.

### `SourceLocation`
- **Packed**: line (20 bits) + column (12 bits)
- **File association**: Stored in `ProgramAST::filePath` and `DeclAST::file`. Not per node.

### `ASTKind`
- Tagged union discriminator – replaces RTTI for fast dispatch.

---

## 2. Top‑Level Structure

### `ProgramAST`
- **Grammar**: `program := package_decl { top_level_decl }` ([Grammar § Top‑Level Structure](/docs/LUC_GRAMMAR.md#top-level-structure))
- **Fields**:
  - `packageName` (InternedString)
  - `filePath` (InternedString)
  - `decls` (ArenaSpan<DeclPtr>)

**Example:**
```luc
package math

use std.io
use math.vec2 as v

struct Vec2 { x float, y float }
let add (a int, b int) -> int = { return a + b }
```

**AST Representation:**
```cpp
ProgramAST {
    packageName = intern("math"),
    filePath = intern("math/vector.luc"),
    decls = [
        PackageDeclAST { name = "math" },
        UseDeclAST { path = ["std", "io"], alias = nullopt },
        UseDeclAST { path = ["math", "vec2"], alias = "v" },
        StructDeclAST { name = "Vec2", fields = [...] },
        FuncDeclAST { name = "add", ... }
    ]
}
```

### `PackageDeclAST`
- **Grammar**: `package_decl := 'package' IDENTIFIER`
- **Derived from**: `DeclAST`
- **Fields**: `name` (InternedString)
- **Note**: Not a `ValueDeclAST` or `TypeDeclAST` – not looked up in symbol tables.

**Example:**
```luc
package renderer
```

**AST Representation:**
```cpp
PackageDeclAST {
    name = intern("renderer"),
    loc = SourceLocation(1, 1)
}
```

### `UseDeclAST`
- **Grammar**: `use_decl := [ visibility_mod ] 'use' module_path [ 'as' IDENTIFIER ]`
- **Derived from**: `DeclAST`
- **Fields**:
  - `path` (ArenaSpan<InternedString>)
  - `alias` (std::optional<InternedString>)
  - `visibility` (Visibility)
- **Note**: Re‑exports (`export use`) are represented by setting `visibility = Export`. No separate node needed.

**Examples:**
```luc
use math.vec2                    -- simple import
use std.io as io                 -- with alias
export use renderer.types        -- re-export
```

**AST Representations:**
```cpp
// use math.vec2
UseDeclAST {
    path = [intern("math"), intern("vec2")],
    alias = nullopt,
    visibility = Private
}

// use std.io as io
UseDeclAST {
    path = [intern("std"), intern("io")],
    alias = intern("io"),
    visibility = Private
}

// export use renderer.types
UseDeclAST {
    path = [intern("renderer"), intern("types")],
    alias = nullopt,
    visibility = Export
}
```

---

## 3. Declarations (Value & Type Namespaces)

### `ValueDeclAST` (abstract)
- Base for declarations that produce a value: variables, functions, parameters, fields, methods, enum variants.
- Caches `valueType` (TypeAST*) – resolved type.
- **Note**: `name` is inherited from `DeclAST`.

### `TypeDeclAST` (abstract)
- Base for type‑defining declarations: struct, enum, trait, type alias.
- Caches:
  - `selfType` (NamedTypeAST*) – a reference to the type itself.
  - `resolvedType` (TypeAST*) – for aliases, the unwrapped underlying type.

---

### 3.1 Variable Declaration

#### `VarDeclAST`
- **Grammar**: `var_decl := [ visibility_mod ] decl_keyword IDENTIFIER type_ann [ '=' expr ]` ([Grammar § Variable Declaration](/docs/LUC_GRAMMAR.md#variable-declaration))
- **Derived from**: `ValueDeclAST`
- **Fields**:
  - `keyword` (DeclKeyword: `Let` or `Const`)
  - `type` (TypeAST*) – original type annotation (may contain aliases)
  - `init` (ExprAST*) – optional initialiser
  - `visibility` (Visibility)

**Examples:**
```luc
let count int = 0
const PI float = 3.14159
let name string? = nil
pub let maxUsers int = 100
```

**AST Representations:**
```cpp
// let count int = 0
VarDeclAST {
    name = intern("count"),
    keyword = Let,
    type = PrimitiveTypeAST { primitiveKind = Int },
    init = LiteralExprAST { kind = Int, value = intern("0") },
    visibility = Private
}

// const PI float = 3.14159
VarDeclAST {
    name = intern("PI"),
    keyword = Const,
    type = PrimitiveTypeAST { primitiveKind = Float },
    init = LiteralExprAST { kind = Float, value = intern("3.14159") },
    visibility = Private
}

// let name string?
VarDeclAST {
    name = intern("name"),
    keyword = Let,
    type = NullableTypeAST {
        inner = PrimitiveTypeAST { primitiveKind = String }
    },
    init = nullptr,
    visibility = Private
}
```

### 3.2 Function Declaration

#### `FuncDeclAST`
- **Grammar**: `func_decl := [ visibility_mod ] decl_keyword IDENTIFIER [ generic_params ] [ qualifier_list ] param_group { param_group } [ '->' return_list ] '=' func_body`
- **Derived from**: `ValueDeclAST`
- **Fields**:
  - `keyword` (DeclKeyword)
  - `genericParams` (ArenaSpan<GenericParamDeclPtr>)
  - `funcType` (FuncTypeAST*) – the full function type
  - `body` (StmtAST*) – always a `BlockStmtAST`
  - `visibility` (Visibility)
  - `resolvedReturnType` (TypeAST*) – cache for first return type

**Examples:**
```luc
let add (a int, b int) -> int = { return a + b }

let fetch ~async (url string) -> string = { return await httpGet(url) }

let identity<T> (v T) -> T = { return v }

let process (a int)(b int) -> (int, string) = {
    return a + b, string(a + b)
}
```

**AST Representations:**
```cpp
// let add (a int, b int) -> int = { return a + b }
FuncDeclAST {
    name = intern("add"),
    keyword = Let,
    genericParams = [],
    funcType = FuncTypeAST {
        params = [
            ParamAST { name = "a", type = PrimitiveTypeAST(Int) },
            ParamAST { name = "b", type = PrimitiveTypeAST(Int) }
        ],
        returnTypes = [ PrimitiveTypeAST(Int) ],
        qualifiers = 0
    },
    body = BlockStmtAST { ... },
    visibility = Private,
    resolvedReturnType = PrimitiveTypeAST(Int)
}

// let identity<T> (v T) -> T
FuncDeclAST {
    name = intern("identity"),
    keyword = Let,
    genericParams = [ GenericParamDeclAST { name = "T", constraints = [] } ],
    funcType = FuncTypeAST {
        params = [ ParamAST { name = "v", type = GenericParamRefAST { name = "T" } } ],
        returnTypes = [ GenericParamRefAST { name = "T" } ]
    },
    // ...
}

// let process (a int)(b int) -> (int, string) — curried
FuncDeclAST {
    name = intern("process"),
    funcType = FuncTypeAST {
        params = [ ParamAST { name = "a", type = PrimitiveTypeAST(Int) } ],
        returnTypes = [ FuncTypeAST {  // nested for currying
            params = [ ParamAST { name = "b", type = PrimitiveTypeAST(Int) } ],
            returnTypes = [
                PrimitiveTypeAST(Int),
                PrimitiveTypeAST(String)
            ]
        } ]
    }
}
```

### 3.3 Parameter

#### `ParamAST`
- **Grammar**: `param := IDENTIFIER type` (within a `param_group`)
- **Derived from**: `ValueDeclAST`
- **Fields**:
  - `type` (TypeAST*) – original type
  - `isVariadic` (bool) – for `...any`

**Examples:**
```luc
(a int, b int)           -- two parameters
(args ...any)            -- variadic parameter
(s string?)              -- nullable parameter
```

**AST Representations:**
```cpp
// a int
ParamAST {
    name = intern("a"),
    type = PrimitiveTypeAST(Int),
    isVariadic = false
}

// args ...any
ParamAST {
    name = intern("args"),
    type = PrimitiveTypeAST(Any),
    isVariadic = true
}

// s string?
ParamAST {
    name = intern("s"),
    type = NullableTypeAST {
        inner = PrimitiveTypeAST(String)
    },
    isVariadic = false
}
```

### 3.4 Struct Declaration

#### `StructDeclAST`
- **Grammar**: `struct_decl := [ visibility_mod ] 'struct' IDENTIFIER [ generic_params ] '{' { field_decl } '}'`
- **Derived from**: `TypeDeclAST`
- **Fields**:
  - `genericParams` (ArenaSpan<GenericParamDeclPtr>)
  - `fields` (ArenaSpan<FieldDeclPtr>)
  - `visibility` (Visibility)

#### `FieldDeclAST`
- **Derived from**: `ValueDeclAST`
- **Fields**:
  - `type` (TypeAST*)
  - `defaultVal` (ExprAST*) – optional default value

**Examples:**
```luc
struct Vec2 {
    x float
    y float
}

struct Rect {
    x float = 0.0
    y float = 0.0
    w float = 1.0
    h float = 1.0
}

struct Box<T> {
    value T
    size uint64 = 0
}
```

**AST Representations:**
```cpp
// struct Vec2 { x float, y float }
StructDeclAST {
    name = intern("Vec2"),
    genericParams = [],
    fields = [
        FieldDeclAST { name = "x", type = PrimitiveTypeAST(Float), defaultVal = nullptr },
        FieldDeclAST { name = "y", type = PrimitiveTypeAST(Float), defaultVal = nullptr }
    ],
    visibility = Private
}

// struct Box<T> { value T, size uint64 = 0 }
StructDeclAST {
    name = intern("Box"),
    genericParams = [ GenericParamDeclAST { name = "T", constraints = [] } ],
    fields = [
        FieldDeclAST {
            name = "value",
            type = GenericParamRefAST { name = "T" },
            defaultVal = nullptr
        },
        FieldDeclAST {
            name = "size",
            type = PrimitiveTypeAST(Uint64),
            defaultVal = LiteralExprAST { kind = Int, value = "0" }
        }
    ]
}
```

### 3.5 Enum Declaration

#### `EnumDeclAST`
- **Grammar**: `enum_decl := [ visibility_mod ] 'enum' IDENTIFIER '{' enum_variant { ',' enum_variant } '}'`
- **Derived from**: `TypeDeclAST`
- **Fields**:
  - `variants` (ArenaSpan<EnumVariantPtr>)
  - `visibility` (Visibility)

#### `EnumVariantAST`
- **Derived from**: `ValueDeclAST`
- **Fields**:
  - `explicitValue` (std::optional<int64_t>)

**Examples:**
```luc
enum Direction { North, South, East, West }

enum ShaderStage {
    Vertex = 0x01
    Fragment = 0x02
    Compute = 0x04
}
```

**AST Representations:**
```cpp
// enum Direction { North, South, East, West }
EnumDeclAST {
    name = intern("Direction"),
    variants = [
        EnumVariantAST { name = "North", explicitValue = nullopt },
        EnumVariantAST { name = "South", explicitValue = nullopt },
        EnumVariantAST { name = "East", explicitValue = nullopt },
        EnumVariantAST { name = "West", explicitValue = nullopt }
    ]
}

// enum ShaderStage { Vertex = 0x01, Fragment = 0x02, Compute = 0x04 }
EnumDeclAST {
    name = intern("ShaderStage"),
    variants = [
        EnumVariantAST { name = "Vertex", explicitValue = 0x01 },
        EnumVariantAST { name = "Fragment", explicitValue = 0x02 },
        EnumVariantAST { name = "Compute", explicitValue = 0x04 }
    ]
}
```

### 3.6 Trait Declaration

#### `TraitDeclAST`
- **Grammar**: `trait_decl := [ visibility_mod ] 'trait' IDENTIFIER [ generic_params ] '{' { trait_method } '}'`
- **Derived from**: `TypeDeclAST`
- **Fields**:
  - `genericParams` (ArenaSpan<GenericParamDeclPtr>)
  - `methods` (ArenaSpan<TraitMethodAST*>)
  - `visibility` (Visibility)

#### `TraitMethodAST`
- **Not** a `ValueDeclAST` – only signature, no body.
- **Fields**:
  - `name` (InternedString)
  - `funcType` (FuncTypeAST*)

**Examples:**
```luc
trait Drawable {
    draw ()
    bounds () -> Rect
}

trait Comparable<T> {
    compareTo (other T) -> int
}
```

**AST Representations:**
```cpp
// trait Drawable { draw (), bounds () -> Rect }
TraitDeclAST {
    name = intern("Drawable"),
    genericParams = [],
    methods = [
        TraitMethodAST {
            name = "draw",
            funcType = FuncTypeAST { params = [], returnTypes = [] }  // void
        },
        TraitMethodAST {
            name = "bounds",
            funcType = FuncTypeAST {
                params = [],
                returnTypes = [ NamedTypeAST { name = "Rect" } ]
            }
        }
    ]
}

// trait Comparable<T> { compareTo (other T) -> int }
TraitDeclAST {
    name = intern("Comparable"),
    genericParams = [ GenericParamDeclAST { name = "T", constraints = [] } ],
    methods = [
        TraitMethodAST {
            name = "compareTo",
            funcType = FuncTypeAST {
                params = [ ParamAST {
                    name = "other",
                    type = GenericParamRefAST { name = "T" }
                } ],
                returnTypes = [ PrimitiveTypeAST(Int) ]
            }
        }
    ]
}
```

### 3.7 Impl Block

#### `ImplDeclAST`
- **Grammar**: `impl_decl := [ visibility_mod ] 'impl' impl_target [ impl_generic_params ] [ 'as' IDENTIFIER ] [ ':' trait_ref ] '{' { method_decl } '}'`
- **Derived from**: `DeclAST`
- **Fields**:
  - `visibility` (Visibility)
  - `targetType` (TypeAST*) – unified target type
  - `methods` (ArenaSpan<MethodDeclPtr>)
  - `genericParams` (ArenaSpan<GenericParamDeclPtr>) – for generic named targets
  - `receiverAlias` (InternedString) – `as` alias, empty means `self`
  - `traitRef` (TraitRefAST*) – optional trait conformance

**Target Type Representations:**

| Source          | `targetType`                                                           | `genericParams`                   |
| --------------- | ---------------------------------------------------------------------- | --------------------------------- |
| `impl int`      | `PrimitiveTypeAST{Int}`                                                | empty                             |
| `impl Vec2`     | `NamedTypeAST{name="Vec2"}`                                            | empty                             |
| `impl [_, int]` | `ArrayTypeAST{kind=Slice, element=PrimitiveTypeAST{Int}}`              | empty                             |
| `impl Box<int>` | `NamedTypeAST{name="Box", genericArgs=[PrimitiveTypeAST{Int}]}`        | empty                             |
| `impl Box<T>`   | `NamedTypeAST{name="Box", genericArgs=[GenericParamRefAST{name="T"}]}` | `[GenericParamDeclAST{name="T"}]` |
| `impl [_, <T>]` | `GenericArrayTypeAST{kind=Slice, typeParamName="T"}`                   | empty                             |

#### `MethodDeclAST`
- **Derived from**: `ValueDeclAST`
- **Fields for inline body**:
  - `funcType` (FuncTypeAST*)
  - `body` (StmtAST*)
- **Fields for assignment forms**:
  - `assignmentRef` (ExprAST*) – function reference
  - `receiverArg` (InternedString)
  - `isInjection` (bool)

**Examples:**
```luc
impl Vec2 {
    length () -> float = { return #sqrt(x*x + y*y) }
}

impl Box<T> as b {
    get () -> T = { return b.value }
}

impl [*, <T>] as a {
    first () -> T = { return a[0] }
}

impl Circle : Drawable {
    draw () = { ... }
    bounds () -> Rect = { ... }
}
```

**AST Representations:**
```cpp
// impl Vec2 { length () -> float = { return #sqrt(x*x + y*y) } }
ImplDeclAST {
    visibility = Private,
    targetType = NamedTypeAST { name = "Vec2" },
    genericParams = [],
    receiverAlias = intern(""),
    traitRef = nullptr,
    methods = [
        MethodDeclAST {
            name = "length",
            funcType = FuncTypeAST {
                params = [],
                returnTypes = [ PrimitiveTypeAST(Float) ]
            },
            body = BlockStmtAST { ... },
            isInjection = false
        }
    ]
}

// impl Box<T> as b { get () -> T = { return b.value } }
ImplDeclAST {
    targetType = NamedTypeAST {
        name = "Box",
        genericArgs = [ GenericParamRefAST { name = "T" } ]
    },
    genericParams = [ GenericParamDeclAST { name = "T" } ],
    receiverAlias = intern("b"),
    methods = [...]
}

// impl [*, <T>] as a { first () -> T = { return a[0] } }
ImplDeclAST {
    targetType = GenericArrayTypeAST {
        arrayKind = Dynamic,
        typeParamName = "T"
    },
    genericParams = [],  // variable embedded in targetType
    receiverAlias = intern("a"),
    methods = [...]
}
```

### 3.8 From Block

#### `FromDeclAST`
- **Grammar**: `from_decl := [ visibility_mod ] 'from' from_target [ generic_params ] '{' { from_entry } '}'`
- **Derived from**: `DeclAST`
- **Fields**:
  - `visibility` (Visibility)
  - `targetType` (TypeAST*) – unified target type
  - `entries` (ArenaSpan<FromEntryPtr>)
  - `genericParams` (ArenaSpan<GenericParamDeclPtr>) – for generic named targets

#### `FromEntryAST`
- **Derived from**: `BaseAST`
- **Fields**:
  - `funcType` (FuncTypeAST*) – conversion signature
  - `body` (StmtAST*) – conversion body

**Examples:**
```luc
from int {
    (s string) -> int = { return #parseInt(s) }
}

from Box<T> {
    (v T) -> Box<T> = { return Box<T> { value = v } }
}

from [_, <T>] {
    (v T) -> [_, T] = { return [v] }
}
```

**AST Representations:**
```cpp
// from int { (s string) -> int = { return #parseInt(s) } }
FromDeclAST {
    visibility = Private,
    targetType = PrimitiveTypeAST(Int),
    genericParams = [],
    entries = [
        FromEntryAST {
            funcType = FuncTypeAST {
                params = [ ParamAST {
                    name = "s",
                    type = PrimitiveTypeAST(String)
                } ],
                returnTypes = [ PrimitiveTypeAST(Int) ]
            },
            body = BlockStmtAST { ... }
        }
    ]
}

// from Box<T> { (v T) -> Box<T> = { return Box<T> { value = v } } }
FromDeclAST {
    targetType = NamedTypeAST {
        name = "Box",
        genericArgs = [ GenericParamRefAST { name = "T" } ]
    },
    genericParams = [ GenericParamDeclAST { name = "T" } ],
    entries = [
        FromEntryAST {
            funcType = FuncTypeAST {
                params = [ ParamAST {
                    name = "v",
                    type = GenericParamRefAST { name = "T" }
                } ],
                returnTypes = [ NamedTypeAST { name = "Box", genericArgs = [GenericParamRefAST{name="T"}] } ]
            },
            body = BlockStmtAST { ... }
        }
    ]
}
```

### 3.9 Type Alias

#### `TypeAliasDeclAST`
- **Grammar**: `type_decl := [ visibility_mod ] 'type' IDENTIFIER [ generic_params ] '=' type_alias_rhs`
- **Derived from**: `TypeDeclAST`
- **Fields**:
  - `genericParams` (ArenaSpan<GenericParamDeclPtr>)
  - `aliasedType` (TypeAST*) – the original alias right‑hand side
  - `visibility` (Visibility)

**Examples:**
```luc
type ID = int
type Callback = (event Event) -> bool
type Option<T> = struct { value T? }
type List<T> = [_, T]
```

**AST Representations:**
```cpp
// type ID = int
TypeAliasDeclAST {
    name = intern("ID"),
    genericParams = [],
    aliasedType = PrimitiveTypeAST(Int),
    visibility = Private
}

// type Option<T> = struct { value T? }
TypeAliasDeclAST {
    name = intern("Option"),
    genericParams = [ GenericParamDeclAST { name = "T" } ],
    aliasedType = StructDeclAST {  // anonymous struct
        fields = [ FieldDeclAST {
            name = "value",
            type = NullableTypeAST {
                inner = GenericParamRefAST { name = "T" }
            }
        } ]
    }
}

// type List<T> = [_, T]
TypeAliasDeclAST {
    name = intern("List"),
    genericParams = [ GenericParamDeclAST { name = "T" } ],
    aliasedType = ArrayTypeAST {
        arrayKind = Slice,
        element = GenericParamRefAST { name = "T" }
    }
}
```

### 3.10 Generic Parameter Nodes

#### `GenericParamDeclAST`
- **Grammar**: `generic_param := IDENTIFIER [ ':' constraint_list ]`
- **Derived from**: `BaseAST`
- **Fields**:
  - `name` (InternedString)
  - `constraints` (ArenaSpan<InternedString>) – trait names
- **Role**: Declares a type variable (e.g., `<T>` in `struct Box<T>`).

#### `GenericParamRefAST`
- **Derived from**: `TypeAST`
- **Fields**:
  - `name` (InternedString)
  - `declaration` (GenericParamDeclAST*) – set during type resolution
  - `isPhantom` (bool)
- **Role**: References a declared generic parameter as a type.

**Example:**
```luc
struct Box<T> {
    value T
    next Box<T>?
}
```

**AST Representation:**
```cpp
// The declaration
GenericParamDeclAST {
    name = intern("T"),
    constraints = []
}

// The uses (GenericParamRefAST)
// In field type: T
GenericParamRefAST {
    name = intern("T"),
    declaration = nullptr,  // set during resolution
    isPhantom = false
}

// In field type: Box<T>
NamedTypeAST {
    name = intern("Box"),
    genericArgs = [ GenericParamRefAST { name = "T" } ]
}
```

---

## 4. Types

All type nodes derive from `TypeAST`. See [Grammar § Types](/docs/LUC_GRAMMAR.md#types).

### `PrimitiveTypeAST`
- **Grammar**: `primitive_type` (bool, int, float, string, etc.)
- **Field**: `primitiveKind` (PrimitiveKind)

**Examples:**
```cpp
PrimitiveTypeAST { primitiveKind = Int }      // int
PrimitiveTypeAST { primitiveKind = String }   // string
PrimitiveTypeAST { primitiveKind = Bool }     // bool
```

### `NamedTypeAST`
- **Grammar**: `IDENTIFIER [ generic_args ]`
- **Fields**:
  - `name` (InternedString)
  - `genericArgs` (ArenaSpan<TypeAST*>)

**Examples:**
```cpp
// Vec2
NamedTypeAST { name = "Vec2", genericArgs = [] }

// Box<int>
NamedTypeAST {
    name = "Box",
    genericArgs = [ PrimitiveTypeAST(Int) ]
}

// Map<string, Vec2>
NamedTypeAST {
    name = "Map",
    genericArgs = [
        PrimitiveTypeAST(String),
        NamedTypeAST { name = "Vec2" }
    ]
}
```

### `GenericParamRefAST`
- See section 3.10 above.

### `NullableTypeAST`
- **Grammar**: `type '?'`
- **Field**: `inner` (TypeAST*)

**Examples:**
```cpp
// int?
NullableTypeAST { inner = PrimitiveTypeAST(Int) }

// Vec2?
NullableTypeAST { inner = NamedTypeAST { name = "Vec2" } }

// Box<int>?
NullableTypeAST {
    inner = NamedTypeAST {
        name = "Box",
        genericArgs = [ PrimitiveTypeAST(Int) ]
    }
}
```

### `ResultTypeAST`
- **Grammar**: `type [ '?' ] '!' [ type ]`
- **Fields**:
  - `inner` (TypeAST*) – success type
  - `errorType` (TypeAST*) – may be nullptr for bare `!`

**Examples:**
```cpp
// int!string
ResultTypeAST {
    inner = PrimitiveTypeAST(Int),
    errorType = PrimitiveTypeAST(String)
}

// int!
ResultTypeAST {
    inner = PrimitiveTypeAST(Int),
    errorType = nullptr
}

// int?!string
ResultTypeAST {
    inner = NullableTypeAST { inner = PrimitiveTypeAST(Int) },
    errorType = PrimitiveTypeAST(String)
}
```

### `ArrayTypeAST`
- **Grammar**: `'[' '_' ',' type ']'` | `'[' '*' ',' type ']'` | `'[' INT_LITERAL ',' type ']'`
- **Fields**:
  - `arrayKind` (ArrayKind: Slice, Dynamic, Fixed)
  - `size` (uint64_t) – only for Fixed
  - `element` (TypeAST*)

**Examples:**
```cpp
// [_, int]
ArrayTypeAST {
    arrayKind = Slice,
    size = 0,
    element = PrimitiveTypeAST(Int)
}

// [*, string]
ArrayTypeAST {
    arrayKind = Dynamic,
    size = 0,
    element = PrimitiveTypeAST(String)
}

// [4, Vec2]
ArrayTypeAST {
    arrayKind = Fixed,
    size = 4,
    element = NamedTypeAST { name = "Vec2" }
}

// [_, [*, int]] — nested
ArrayTypeAST {
    arrayKind = Slice,
    element = ArrayTypeAST {
        arrayKind = Dynamic,
        element = PrimitiveTypeAST(Int)
    }
}
```

### `GenericArrayTypeAST`
- **Grammar**: `'[' '_' ',' '<' IDENTIFIER '>' ']'` etc.
- **Used only** as `impl`/`from` target.
- **Fields**:
  - `arrayKind` (ArrayKind)
  - `size` (uint64_t) – for Fixed arrays
  - `typeParamName` (InternedString)

**Examples:**
```cpp
// [_, <T>]
GenericArrayTypeAST {
    arrayKind = Slice,
    size = 0,
    typeParamName = "T"
}

// [*, <E>]
GenericArrayTypeAST {
    arrayKind = Dynamic,
    size = 0,
    typeParamName = "E"
}

// [4, <N>]
GenericArrayTypeAST {
    arrayKind = Fixed,
    size = 4,
    typeParamName = "N"
}
```

### `RefTypeAST`
- **Grammar**: `'&' type`
- **Field**: `inner` (TypeAST*)

**Examples:**
```cpp
// &int
RefTypeAST { inner = PrimitiveTypeAST(Int) }

// &Vec2
RefTypeAST { inner = NamedTypeAST { name = "Vec2" } }

// &Vec2? — nullable reference
NullableTypeAST {
    inner = RefTypeAST { inner = NamedTypeAST { name = "Vec2" } }
}
```

### `PtrTypeAST`
- **Grammar**: `'*' type`
- **Field**: `inner` (TypeAST*)

**Examples:**
```cpp
// *uint8
PtrTypeAST { inner = PrimitiveTypeAST(Uint8) }

// *Node?
NullableTypeAST {
    inner = PtrTypeAST { inner = NamedTypeAST { name = "Node" } }
}
```

### `FuncTypeAST`
- **Grammar**: `[ qualifier_list ] param_group { param_group } [ '->' return_list ]`
- **Fields**:
  - `params` (ArenaSpan<ParamAST*>)
  - `returnTypes` (ArenaSpan<TypeAST*>)
  - `qualifiers` (uint32_t)
  - `rawQualifiers` (ArenaSpan<InternedString>)

**Examples:**
```cpp
// (a int, b int) -> int
FuncTypeAST {
    params = [
        ParamAST { name = "a", type = PrimitiveTypeAST(Int) },
        ParamAST { name = "b", type = PrimitiveTypeAST(Int) }
    ],
    returnTypes = [ PrimitiveTypeAST(Int) ],
    qualifiers = 0
}

// ~async (url string) -> string
FuncTypeAST {
    params = [ ParamAST { name = "url", type = PrimitiveTypeAST(String) } ],
    returnTypes = [ PrimitiveTypeAST(String) ],
    qualifiers = QualifierBits::Async,
    rawQualifiers = [intern("async")]
}

// (a int) -> (b int) -> int — curried
FuncTypeAST {
    params = [ ParamAST { name = "a", type = PrimitiveTypeAST(Int) } ],
    returnTypes = [ FuncTypeAST {
        params = [ ParamAST { name = "b", type = PrimitiveTypeAST(Int) } ],
        returnTypes = [ PrimitiveTypeAST(Int) ]
    } ]
}
```

---

## 5. Expressions

All expression nodes derive from `ExprAST`. Common fields: `resolvedType`, `isBehaviorMember`, `isConst`.

### Literals

#### `LiteralExprAST`
- **Fields**: `kind` (LiteralKind), `value` (InternedString)

**Examples:**
```cpp
LiteralExprAST { kind = Int, value = intern("42") }
LiteralExprAST { kind = Float, value = intern("3.14159") }
LiteralExprAST { kind = String, value = intern("hello") }
LiteralExprAST { kind = Bool, value = intern("true") }
LiteralExprAST { kind = Nil, value = intern("nil") }
```

#### `ArrayLiteralExprAST`
- **Fields**: `elements` (ArenaSpan<ExprAST*>)

**Example:**
```luc
[1, 2, 3]
```

```cpp
ArrayLiteralExprAST {
    elements = [
        LiteralExprAST { kind = Int, value = "1" },
        LiteralExprAST { kind = Int, value = "2" },
        LiteralExprAST { kind = Int, value = "3" }
    ]
}
```

#### `StructLiteralExprAST`
- **Fields**:
  - `typeName` (InternedString)
  - `genericArgs` (ArenaSpan<TypeAST*>)
  - `inits` (ArenaSpan<FieldInitPtr>)
  - `instantiatedType` (NamedTypeAST*) – cache

**Example:**
```luc
Vec2 { x = 1.0, y = 2.0 }
```

```cpp
StructLiteralExprAST {
    typeName = intern("Vec2"),
    genericArgs = [],
    inits = [
        FieldInitAST { name = "x", value = LiteralExprAST(Float, "1.0") },
        FieldInitAST { name = "y", value = LiteralExprAST(Float, "2.0") }
    ],
    instantiatedType = nullptr
}
```

### Names & Access

#### `IdentifierExprAST`
- **Fields**: `name` (InternedString), `genericArgs` (ArenaSpan<TypeAST*>)

**Examples:**
```cpp
// x
IdentifierExprAST { name = intern("x"), genericArgs = [] }

// identity<int>
IdentifierExprAST {
    name = intern("identity"),
    genericArgs = [ PrimitiveTypeAST(Int) ]
}
```

#### `FieldAccessExprAST`
- **Fields**: `object` (ExprAST*), `field` (InternedString), `genericArgs` (ArenaSpan<TypeAST*>)

**Examples:**
```cpp
// v.x
FieldAccessExprAST {
    object = IdentifierExprAST { name = "v" },
    field = intern("x"),
    genericArgs = []
}

// utils.toString<int>
FieldAccessExprAST {
    object = IdentifierExprAST { name = "utils" },
    field = intern("toString"),
    genericArgs = [ PrimitiveTypeAST(Int) ]
}
```

#### `BehaviorAccessExprAST`
- **Fields**: `object` (ExprAST*), `method` (InternedString)

**Examples:**
```cpp
// v:normalize
BehaviorAccessExprAST {
    object = IdentifierExprAST { name = "v" },
    method = intern("normalize")
}
```

### Calls

#### `CallExprAST`
- **Fields**:
  - `callee` (ExprAST*)
  - `genericArgs` (ArenaSpan<TypeAST*>)
  - `args` (ArenaSpan<ExprAST*>)
  - `callKind` (CallKind)

**Examples:**
```cpp
// f(1, 2)
CallExprAST {
    callee = IdentifierExprAST { name = "f" },
    genericArgs = [],
    args = [
        LiteralExprAST(Int, "1"),
        LiteralExprAST(Int, "2")
    ],
    callKind = Plain
}

// identity<int>(42)
CallExprAST {
    callee = IdentifierExprAST { name = "identity", genericArgs = [PrimitiveTypeAST(Int)] },
    genericArgs = [],  // already applied to callee
    args = [ LiteralExprAST(Int, "42") ],
    callKind = Plain
}

// await fetch(url)
CallExprAST {
    callee = IdentifierExprAST { name = "fetch" },
    args = [ IdentifierExprAST { name = "url" } ],
    callKind = Async  // indicates ~async, must be awaited
}
```

### Operators

#### `BinaryExprAST`
- **Fields**: `op` (BinaryOp), `left` (ExprAST*), `right` (ExprAST*)

**Examples:**
```cpp
// a + b
BinaryExprAST {
    op = Add,
    left = IdentifierExprAST { name = "a" },
    right = IdentifierExprAST { name = "b" }
}

// x == y
BinaryExprAST {
    op = Eq,
    left = IdentifierExprAST { name = "x" },
    right = IdentifierExprAST { name = "y" }
}
```

#### `UnaryExprAST`
- **Fields**: `op` (UnaryOp), `operand` (ExprAST*)

**Examples:**
```cpp
// -x
UnaryExprAST {
    op = Neg,
    operand = IdentifierExprAST { name = "x" }
}

// not flag
UnaryExprAST {
    op = Not,
    operand = IdentifierExprAST { name = "flag" }
}

// &value
UnaryExprAST {
    op = Ref,
    operand = IdentifierExprAST { name = "value" }
}
```

#### `AssignExprAST`
- **Fields**: `op` (AssignOp), `lhs` (ExprAST*), `rhs` (ExprAST*)

**Examples:**
```cpp
// x = 5
AssignExprAST {
    op = Assign,
    lhs = IdentifierExprAST { name = "x" },
    rhs = LiteralExprAST(Int, "5")
}

// x += 1
AssignExprAST {
    op = AddAssign,
    lhs = IdentifierExprAST { name = "x" },
    rhs = LiteralExprAST(Int, "1")
}
```

### Nullable

#### `NullableChainExprAST`
- **Fields**: `object` (ExprAST*), `steps` (ArenaSpan<InternedString>)
- **Note**: Must be terminated by `??` (grammar requirement)

**Example:**
```luc
player?.weapon?.damage ?? 0
```

```cpp
NullableChainExprAST {
    object = IdentifierExprAST { name = "player" },
    steps = [intern("weapon"), intern("damage")]
}
// The ?? 0 is a separate NullCoalesceExprAST wrapping this chain
```

#### `NullCoalesceExprAST`
- **Fields**: `value` (ExprAST*), `fallback` (ExprAST*)

**Example:**
```luc
value ?? 0
```

```cpp
NullCoalesceExprAST {
    value = IdentifierExprAST { name = "value" },
    fallback = LiteralExprAST(Int, "0")
}
```

### Type Test

#### `IsExprAST`
- **Fields**: `expr` (ExprAST*), `checkType` (TypeAST*)

**Examples:**
```luc
x is int
shape is Circle
shape is Drawable
```

```cpp
// x is int
IsExprAST {
    expr = IdentifierExprAST { name = "x" },
    checkType = PrimitiveTypeAST(Int)
}

// shape is Circle
IsExprAST {
    expr = IdentifierExprAST { name = "shape" },
    checkType = NamedTypeAST { name = "Circle" }
}
```

### Pipelines

#### `PipelineExprAST`
- **Fields**: `seed` (ExprAST*), `steps` (ArenaSpan<PipelineStepPtr>)

#### `PipelineStepAST`
- **Fields**: `callable` (ExprAST*), `packArgs` (ArenaSpan<ExprAST*>)

**Example:**
```luc
42 |> float |> sqrt
```

```cpp
PipelineExprAST {
    seed = LiteralExprAST(Int, "42"),
    steps = [
        PipelineStepAST {
            callable = IdentifierExprAST { name = "float" },
            packArgs = []
        },
        PipelineStepAST {
            callable = IdentifierExprAST { name = "sqrt" },
            packArgs = []
        }
    ]
}
```

### Composition

#### `ComposeExprAST`
- **Fields**: `left` (ExprAST*), `operands` (ArenaSpan<ComposeOperandPtr>)

#### `ComposeOperandAST`
- **Fields**: `callable` (ExprAST*), `genericArgs` (ArenaSpan<TypeAST*>)

**Example:**
```luc
validate +> transform +> render
```

```cpp
ComposeExprAST {
    left = IdentifierExprAST { name = "validate" },
    operands = [
        ComposeOperandAST { callable = IdentifierExprAST { name = "transform" } },
        ComposeOperandAST { callable = IdentifierExprAST { name = "render" } }
    ]
}
```

### Anonymous Functions

#### `AnonFuncExprAST`
- **Fields**: `funcType` (FuncTypeAST*), `body` (StmtAST*)

**Example:**
```luc
(x int) -> int { return x * 2 }
```

```cpp
AnonFuncExprAST {
    funcType = FuncTypeAST {
        params = [ ParamAST { name = "x", type = PrimitiveTypeAST(Int) } ],
        returnTypes = [ PrimitiveTypeAST(Int) ]
    },
    body = BlockStmtAST { ... }
}
```

### Await

#### `AwaitExprAST`
- **Fields**: `inner` (ExprAST*)

**Example:**
```luc
await fetch(url)
```

```cpp
AwaitExprAST {
    inner = CallExprAST {
        callee = IdentifierExprAST { name = "fetch" },
        args = [ IdentifierExprAST { name = "url" } ]
    }
}
```

### Resolve

#### `ResolveExprAST`
- **Fields**: `subject` (ExprAST*), `okArm` (OkArmAST*), `errArm` (ErrArmAST*)

#### `OkArmAST`
- **Fields**: `bindName` (InternedString), `bindType` (TypeAST*), `body` (StmtAST*)

#### `ErrArmAST`
- **Fields**: `bindName` (InternedString), `bindType` (TypeAST*), `body` (StmtAST*)

**Example:**
```luc
resolve divide(10, 0) {
    ok  (v int)    { return v }
    err (e string) { return -1 }
}
```

```cpp
ResolveExprAST {
    subject = CallExprAST { ... },
    okArm = OkArmAST {
        bindName = "v",
        bindType = PrimitiveTypeAST(Int),
        body = BlockStmtAST { ... }
    },
    errArm = ErrArmAST {
        bindName = "e",
        bindType = PrimitiveTypeAST(String),
        body = BlockStmtAST { ... }
    }
}
```

### Match Expression

#### `MatchExprAST`
- **Fields**:
  - `subject` (ExprAST*)
  - `arms` (ArenaSpan<MatchArmPtr>)
  - `defaultBody` (DefaultArmAST*)
  - `defaultLoc` (SourceLocation)

#### `MatchArmAST`
- **Fields**: `patterns` (ArenaSpan<PatternAST*>), `guard` (ExprAST*), `exprs` (ArenaSpan<ExprAST*>)

**Example:**
```luc
match status {
    200, 201      => "ok"
    404           => "not found"
    n if n < 500  => "error: " + string(n)
    default       => "unknown"
}
```

```cpp
MatchExprAST {
    subject = IdentifierExprAST { name = "status" },
    arms = [
        MatchArmAST {
            patterns = [ PatternExprAST { inner = LiteralExprAST(Int, "200") },
                         PatternExprAST { inner = LiteralExprAST(Int, "201") } ],
            guard = nullptr,
            exprs = [ LiteralExprAST(String, "ok") ]
        },
        MatchArmAST {
            patterns = [ PatternExprAST { inner = LiteralExprAST(Int, "404") } ],
            exprs = [ LiteralExprAST(String, "not found") ]
        },
        MatchArmAST {
            patterns = [ BindPatternAST { name = "n" } ],
            guard = BinaryExprAST {
                op = Lt,
                left = IdentifierExprAST { name = "n" },
                right = LiteralExprAST(Int, "500")
            },
            exprs = [ CallExprAST { ... } ]
        }
    ],
    defaultBody = DefaultArmAST { exprs = [ LiteralExprAST(String, "unknown") ] }
}
```

### Intrinsic Call

#### `IntrinsicCallExprAST`
- **Fields**: `intrinsicName` (InternedString), `args` (ArenaSpan<ExprAST*>)

**Examples:**
```luc
#sizeof(Vertex)
#sqrt(x)
#ptrOffset(ptr, 1)
```

```cpp
IntrinsicCallExprAST {
    intrinsicName = intern("sizeof"),
    args = [ NamedTypeAST { name = "Vertex" } ]  // types as expressions
}
```

---

## 6. Statements

All statement nodes derive from `StmtAST`. See [Grammar § Statements](/docs/LUC_GRAMMAR.md#statements).

### Block

#### `BlockStmtAST`
- **Fields**: `stmts` (ArenaSpan<StmtAST*>)

**Example:**
```luc
{
    let x int = 10
    io.printl(x)
}
```

```cpp
BlockStmtAST {
    stmts = [
        DeclStmtAST { decl = VarDeclAST { ... } },
        ExprStmtAST { expr = CallExprAST { ... } }
    ]
}
```

### Declaration Statement

#### `DeclStmtAST`
- **Fields**: `decl` (DeclAST*)

**Example:**
```luc
let compute () -> int = { return 42 }
```

```cpp
DeclStmtAST {
    decl = FuncDeclAST { ... }
}
```

### Expression Statement

#### `ExprStmtAST`
- **Fields**: `expr` (ExprAST*)

**Example:**
```luc
io.printl("done")
```

```cpp
ExprStmtAST {
    expr = CallExprAST { ... }
}
```

### If Statement

#### `IfStmtAST`
- **Fields**: `condition` (ExprAST*), `thenBranch` (StmtAST*), `elseBranch` (StmtAST*)

**Examples:**
```luc
if score >= 90 { io.printl("A") }

if score >= 90 { io.printl("A") } else { io.printl("F") }

if x < 0 { return } else if x == 0 { io.printl("zero") } else { io.printl("positive") }
```

```cpp
IfStmtAST {
    condition = BinaryExprAST { op = Ge, ... },
    thenBranch = BlockStmtAST { ... },
    elseBranch = nullptr
}

IfStmtAST {
    condition = BinaryExprAST { ... },
    thenBranch = BlockStmtAST { ... },
    elseBranch = BlockStmtAST { ... }
}
```

### Switch Statement

#### `SwitchStmtAST`
- **Fields**:
  - `subject` (ExprAST*)
  - `cases` (ArenaSpan<SwitchCasePtr>)
  - `defaultBody` (BlockStmtAST*)
  - `defaultLoc` (std::optional<SourceLocation>)

#### `SwitchCaseAST`
- **Fields**: `values` (ArenaSpan<ExprAST*>), `body` (BlockStmtAST*)

**Example:**
```luc
switch code {
    case 200, 201: { io.printl("ok") }
    case 1..10:    { io.printl("light") }
    default:       { io.printl("other") }
}
```

```cpp
SwitchStmtAST {
    subject = IdentifierExprAST { name = "code" },
    cases = [
        SwitchCaseAST {
            values = [
                LiteralExprAST(Int, "200"),
                LiteralExprAST(Int, "201")
            ],
            body = BlockStmtAST { ... }
        },
        SwitchCaseAST {
            values = [ RangeExprAST { lo = 1, hi = 10 } ],
            body = BlockStmtAST { ... }
        }
    ],
    defaultBody = BlockStmtAST { ... }
}
```

### For Loop

#### `ForStmtAST`
- **Fields**: `iterVar` (ParamAST*), `iterable` (ExprAST*), `step` (ExprAST*), `body` (StmtAST*)

**Examples:**
```luc
for i int in 0..10 { io.printl(string(i)) }

for item string in items { process(item) }

for i int in 0..10..2 { io.printl(string(i)) }
```

```cpp
ForStmtAST {
    iterVar = ParamAST { name = "i", type = PrimitiveTypeAST(Int) },
    iterable = RangeExprAST { lo = 0, hi = 10, isExclusive = false },
    step = nullptr,
    body = BlockStmtAST { ... }
}

ForStmtAST {
    iterVar = ParamAST { name = "item", type = PrimitiveTypeAST(String) },
    iterable = IdentifierExprAST { name = "items" },
    step = nullptr,
    body = BlockStmtAST { ... }
}
```

### While Loop

#### `WhileStmtAST`
- **Fields**: `condition` (ExprAST*), `body` (StmtAST*)

**Example:**
```luc
while n < 5 { n += 1 }
```

```cpp
WhileStmtAST {
    condition = BinaryExprAST { op = Lt, ... },
    body = BlockStmtAST { ... }
}
```

### Do-While Loop

#### `DoWhileStmtAST`
- **Fields**: `body` (StmtAST*), `condition` (ExprAST*)

**Example:**
```luc
do { n += 1 } while n < 5
```

```cpp
DoWhileStmtAST {
    body = BlockStmtAST { ... },
    condition = BinaryExprAST { op = Lt, ... }
}
```

### Return Statement

#### `ReturnStmtAST`
- **Fields**: `values` (ArenaSpan<ExprAST*>)

**Examples:**
```luc
return
return 42
return a + b
```

```cpp
ReturnStmtAST { values = [] }                     // bare return
ReturnStmtAST { values = [ LiteralExprAST(Int, "42") ] }
ReturnStmtAST { values = [ BinaryExprAST { ... } ] }
```

### Break / Continue

#### `BreakStmtAST`
- No fields.

#### `ContinueStmtAST`
- No fields.

### Multi-Variable Declaration

#### `MultiVarDeclAST`
- **Fields**:
  - `keyword` (DeclKeyword)
  - `vars` (ArenaSpan<std::pair<InternedString, TypeAST*>>)
  - `rhs` (ExprAST*)

**Example:**
```luc
let q int, r int = divmod(10, 3)
```

```cpp
MultiVarDeclAST {
    keyword = Let,
    vars = [
        { intern("q"), PrimitiveTypeAST(Int) },
        { intern("r"), PrimitiveTypeAST(Int) }
    ],
    rhs = CallExprAST {
        callee = IdentifierExprAST { name = "divmod" },
        args = [
            LiteralExprAST(Int, "10"),
            LiteralExprAST(Int, "3")
        ]
    }
}
```

### Multi-Assignment Statement

#### `MultiAssignStmtAST`
- **Fields**: `lhs` (ArenaSpan<ExprAST*>), `rhs` (ExprAST*)

**Example:**
```luc
a, b = f()
```

```cpp
MultiAssignStmtAST {
    lhs = [
        IdentifierExprAST { name = "a" },
        IdentifierExprAST { name = "b" }
    ],
    rhs = CallExprAST {
        callee = IdentifierExprAST { name = "f" }
    }
}
```

---

## 7. Attributes

Attributes are represented by simple AST nodes, but their validation is delegated to a dedicated registry (`AttributeRegistry`). The nodes exist only to carry raw data.

### `AttributeAST`
- **Grammar**: `'@' IDENTIFIER [ '(' attr_arg_list ')' ]`
- **Fields**: `name` (InternedString), `args` (ArenaSpan<AttributeArgPtr>)

### `AttributeArgAST`
- **Fields**: `kind` (AttributeArgKind), `value` (InternedString)

**Examples:**
```luc
@inline
@deprecated("use newFunction")
@extern("malloc", "C")
```

```cpp
AttributeAST { name = intern("inline"), args = [] }

AttributeAST {
    name = intern("deprecated"),
    args = [ AttributeArgAST { kind = StringLit, value = intern("use newFunction") } ]
}

AttributeAST {
    name = intern("extern"),
    args = [
        AttributeArgAST { kind = StringLit, value = intern("malloc") },
        AttributeArgAST { kind = TypeIdent, value = intern("C") }
    ]
}
```

---

## 8. Known Design Gaps & Improvement Suggestions

### 8.1 Structural Improvements

1. **`?` and `!` restrictions** – Grammar disallows certain patterns; semantic passes must enforce them.

2. **`GenericArrayTypeAST` placement** – Valid only as `impl`/`from` target; semantic pass must reject elsewhere.

### 8.2 Performance Notes

- All variable‑length lists use `ArenaSpan` – zero heap overhead.
- All identifiers use `InternedString` – 4‑byte handles, O(1) comparison.
- Type dispatch uses `ASTKind` enum and `switch` – no virtual function overhead.

---

## 9. Summary

The AST nodes faithfully represent the entire Luc grammar. The current design is production‑ready for parser and initial semantic passes. Key design points:

- **Generic parameters**: `GenericParamDeclAST` for declaration, `GenericParamRefAST` for reference
- **Impl/From targets**: Unified `targetType` with `genericParams` for generic named targets
- **From entries**: Inline only (path entries removed)
- **Expressions**: Full support for literals, operators, calls, pipelines, composition, and control flow
- **Statements**: Complete coverage of blocks, branches, loops, jumps, and multi-assignments
