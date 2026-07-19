# Lucid Compiler — Architecture

> This document describes the internal architecture of the Lucid
> Compiler: how the source code flows from text to execution, how the major
> subsystems are structured, and how each folder in the codebase fits into
> that flow.

---

## Table of Contents

- [Lucid Compiler — Architecture](#lucid-compiler--architecture)
  - [Table of Contents](#table-of-contents)
  - [1. Overview](#1-overview)
  - [2. Execution Pipeline](#2-execution-pipeline)
  - [3. Frontend](#3-frontend)
    - [3.1 Lexer](#31-lexer)
    - [3.2 Parser](#32-parser)
    - [3.3 Semantic Analysis](#33-semantic-analysis)
  - [4. IR Lowering](#4-ir-lowering)
  - [5. Backend — Interpreter (ORC JIT)](#5-backend--interpreter-orc-jit)
    - [JIT Session Setup](#jit-session-setup)
    - [Foreign Symbol Resolution](#foreign-symbol-resolution)
    - [Hot-Reload](#hot-reload)
  - [6. Backend — Compiler (AOT)](#6-backend--compiler-aot)
  - [7. Runtime](#7-runtime)
    - [Memory (`memory.hpp/cpp`)](#memory-memoryhppcpp)
    - [Threading (`threading.hpp/cpp`)](#threading-threadinghppcpp)
    - [FFI (`ffi/`)](#ffi-ffi)
  - [8. Standard Library](#8-standard-library)
  - [9. CLI and LSP](#9-cli-and-lsp)
    - [CLI (`src/cli/`)](#cli-srccli)
    - [LSP (`src/lsp/`)](#lsp-srclsp)
  - [10. File Structure](#10-file-structure)

---

## 1. Overview

The Lucid Compiler is a single binary (`lucid`) that serves two
modes from a shared frontend:

- **`lucid run`** — interprets a `.luc` file immediately using LLVM's ORC
  JIT. No output file is produced. The user sees results instantly without
  any compiler installation step.
- **`lucid build`** — compiles `.luc` files ahead-of-time (AOT) using LLVM
  and the system linker, producing a native binary or shared library.

Both modes share the same frontend (lexer, parser, semantic analysis) and the
same IR lowering pass. The only difference is what happens to the LLVM IR
after it is produced: the JIT compiles it in memory and executes it; the AOT
path writes it to an object file and invokes the system linker.

```
                    ┌─────────────────┐
                    │  Lucid Source   │
                    │   (.luc)      │
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │     Lexer       │  tokens
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │     Parser      │  AST
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │    Semantic     │  validated AST
                    │    Analysis     │
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │   IR Lowering   │  LLVM IR
                    └────────┬────────┘
                             │
              ┌──────────────┴──────────────┐
              │                             │
   ┌──────────▼──────────┐     ┌────────────▼────────────┐
   │    ORC JIT          │     │    AOT Compiler          │
   │  (lucid run)        │     │  (lucid build)           │
   │                     │     │                          │
   │  compile IR         │     │  emit object file        │
   │  in memory          │     │  invoke system linker    │
   │  dlopen @[link]     │     │  -l flags from @[link]   │
   │  execute            │     │  produce native binary   │
   └─────────────────────┘     └──────────────────────────┘
```

---

> **NOTE — LLVM is statically bundled, not a user-installed dependency.**
> Because the compiler ships inside the Engine SDK and a game developer is
> never expected to have LLVM on their machine, `lucid` links LLVM
> statically rather than against a system install. This makes the binary
> itself the main thing worth watching for size — the same levers used to
> keep `luc_kernel.dll` lean apply here instead of a separate build doc:
> - **Target backend count** is the single biggest driver. Supporting only
>   the platforms actually shipped to (e.g. one X86 backend covering both
>   Windows/COFF and Linux/ELF object emission) keeps this small; each
>   additional backend (AArch64 for future mobile/Apple Silicon) adds real
>   weight.
> - **JIT (ORC) and AOT share the same IR lowering** and most of the same
>   LLVM codegen, so supporting both `run` and `build` costs far less than
>   the first backend does — it's not roughly double.
> - Standard `-ffunction-sections -fdata-sections` + `--gc-sections`/`/OPT:REF`
>   and symbol stripping apply to this binary exactly as they do to the
>   kernel — LLVM is one more static library being linked, not a special case.
> - If `luc_langserver` ends up as a separate binary from `lucid`, it only
>   needs the frontend (lexer/parser/sema) for diagnostics and autocomplete —
>   it never touches `IRLowering`, so it doesn't need to link LLVM at all.
>   Keeping it LLVM-free is worth preserving as a deliberate constraint,
>   not just an implementation detail.

---

## 2. Execution Pipeline

A complete walk through what happens when the user runs `lucid run main.luc`:

**Step 1 — Module Resolution**
The `ModuleResolver` scans all source files referenced by `main.luc`
(via import declarations), builds a dependency graph, checks for cyclic
dependencies, and produces a topologically ordered list of modules to process.

**Step 2 — Lexing**
The `Lexer` reads each source file character by character and produces a flat
stream of `Token` values. Whitespace and comments are stripped. Keywords,
identifiers, literals, and operators become typed tokens. The `TokenStream`
provides the parser with a buffered, lookahead-capable view of this stream.

**Step 3 — Parsing**
The `Parser` consumes the `TokenStream` and builds an Abstract Syntax Tree
(AST). The AST is allocated in an `ASTArena` — a bump-pointer memory pool
that makes allocation fast and frees the entire tree in one operation when
the stage is done. Identifiers are stored as interned strings via
`StringPool` to enable O(1) equality comparison throughout the pipeline.

**Step 4 — Semantic Analysis**
The `Sema` pass walks the AST and validates it:
- `NameResolver` resolves every identifier to its declaration.
- `ScopeManager` enforces lexical scoping rules.
- `TypeChecker` infers and validates types, checks assignment compatibility,
  and validates function call argument types.
- `FFIValidator` checks every `@[foreign("C")]` declaration against the
  `lge_ffi.lfi` symbol table — parameter types and return types must match
  the C ABI rules defined in the grammar.

Errors from semantic analysis are collected and reported via `Diagnostics`
before execution begins. No IR is generated until the AST is clean.

**Step 5 — IR Lowering**
`IRLowering` translates the validated AST to LLVM IR. Every Lucid construct
maps to a specific IR pattern:
- Lucid functions → LLVM `define`
- `@[foreign("C")]` declarations → LLVM `declare` (unresolved external)
- Lucid `#intrinsics` → LLVM intrinsic calls (`llvm.sqrt`, `llvm.memcpy`, etc.)
- Lucid types → LLVM types via `TypeMapping`

This step is identical for both `run` and `build`. The IR produced here is
the handoff point between the frontend and the two backends.

**Step 6a — ORC JIT (run mode)**
The `JIT` session receives the LLVM IR module. It compiles it to native
machine code in memory. For every `@[link("libname")]` annotation encountered,
`DynLink` calls `dlopen` / `LoadLibrary` to load the named shared library and
registers its exported symbols with the JIT's symbol table. The JIT resolves
the `declare` stubs against this table and executes the module. Hot-reload
works by recompiling a changed module and replacing the old one in the JIT
session between frames.

**Step 6b — AOT Compiler (build mode)**
The `AOT` backend receives the same LLVM IR module. It runs the LLVM
optimisation pipeline and emits a native object file. The `Linker` collects
all `@[link(...)]` annotations from the module and constructs a linker
invocation (`ld`, `lld`, or `link.exe` depending on platform), passing
them as `-l` flags. The result is a native binary or shared library.

---

## 3. Frontend

The frontend is the entire pipeline from source text to validated AST. It is
shared between both modes and never touches LLVM. All three stages (lexer,
parser, sema) must complete successfully before any IR is produced.

### 3.1 Lexer

**Location:** `src/parser/lexer/`

The lexer transforms raw source text into a stream of tokens. It is a single
pass over the input with no backtracking. Every `Token` carries:
- A `TokenKind` enum value (keyword, identifier, literal, operator, etc.)
- A `SourceLocation` (file, line, column) for error reporting
- A string payload for identifiers and literals

The `TokenStream` wraps the raw token stream and gives the parser:
- `peek(n)` — lookahead n tokens without consuming
- `advance()` — consume and return the current token
- `expect(kind)` — consume and assert kind, or emit a diagnostic

**Key files:**
- `Lexer.hpp/cpp` — the lexer itself: character scanning, token production
- `TokenStream.hpp/cpp` — buffered stream with lookahead

### 3.2 Parser

**Location:** `src/parser/`

The parser consumes the `TokenStream` and produces an AST. The grammar is
encoded directly in the parsing functions — no grammar table, no generated
parser. The top-level entry points are `Parser::parse()` (single module)
and `Parser::parseFile()` (entry point that drives `ModuleResolver`).

The expression parser uses a **Pratt parser** (top-down operator precedence).
This handles operator precedence and associativity cleanly without a grammar
table: each operator has a binding power, and the parser recurses based on
those powers. This is defined in `rules/ParserExpr.cpp`.

`ParserContext` is the shared state threaded through all parsing functions:
the current token stream, the arena allocator, the string pool, the
diagnostic sink, and the current parse flags (e.g. whether we are inside an
async context).

**Key files:**
- `Parser.hpp/cpp` — public interface: `parse()`, `parseFile()`
- `ModuleResolver.hpp/cpp` — resolves multi-file projects, enforces acyclic imports
- `support/ParserContext.hpp` — shared parse state
- `support/ErrorRecovery.cpp` — synchronisation points for error recovery
- `rules/ParserDecl.cpp` — `const`, `let`, `struct`, `enum`, `trait`, function declarations
- `rules/ParserStmt.cpp` — `if`, `for`, `while`, `return`, expression statements
- `rules/ParserExpr.cpp` — Pratt parser for all expressions
- `rules/ParserType.cpp` — type annotation parsing (`*T`, `T?`, `Result<T, F>`, etc.)
- `rules/ParserConcurrency.cpp` — `async`, `parallel`, `await`, `spawn`, `join`
- `support/Lookahead.cpp` — lookahead decision helpers (disambiguate grammar points)
- `support/Helpers.cpp` — shared helpers: attribute parsing (`@[...]`), doc comments

**AST node categories** (defined in `core/ast/`):
- `DeclAST` — declarations: functions, variables, structs, enums, traits, modules
- `StmtAST` — statements: if, for, while, return, block
- `ExprAST` — expressions: binary, unary, call, index, field access, literals
- `TypeAST` — type annotations: primitives, pointers, generics, function types

### 3.3 Semantic Analysis

**Location:** `src/sema/`

Semantic analysis is a multi-pass walk over the validated AST. The passes
run in order; each pass may annotate AST nodes with resolved types and
declaration references. All errors are collected and reported together at
the end of this stage — the compiler never produces IR for a file with
semantic errors.

**Passes in order:**

1. **NameResolver** — first pass. Builds the declaration map for each scope.
   Resolves every identifier node to the declaration it refers to. Detects
   use-before-declaration and undefined names.

2. **TypeChecker** — second pass. Walks the AST and assigns a resolved type
   to every expression node. Validates that assignments, function arguments,
   and return statements match the declared types. Handles the Lucid type
   rules: nullable (`T?`), fallible (`T!`), reference (`&T`), raw pointer
   (`*T`), and the `Result<T, Flag, Payload<U>>` generic family.

3. **FFIValidator** — third pass, runs only on `@[foreign("C")]`
   declarations. Loads the `lge_ffi.lfi` symbol table and cross-references
   each foreign declaration:
   - Symbol name must exist in the table
   - Parameter count must match
   - Each parameter type must be ABI-compatible with the C type in the table
   - Return type must be ABI-compatible
   Mismatches produce a diagnostic with the expected C signature alongside
   the declared Lucid signature so the developer can see exactly what differs.

`SemaContext` carries the shared state for all passes: the resolved symbol
table, the current scope stack, the type environment, and the diagnostic sink.

**Key files:**
- `Sema.hpp/cpp` — entry point: `Sema::analyze(Module*)` runs all passes
- `NameResolver.hpp/cpp`
- `TypeChecker.hpp/cpp`
- `ScopeManager.hpp/cpp` — push/pop scopes, lookup with shadowing rules
- `FFIValidator.hpp/cpp`
- `SemaContext.hpp` — shared state threaded through all passes

---

## 4. IR Lowering

**Location:** `src/compiler/`

IR Lowering translates the validated AST to LLVM IR. This is the **only**
place in the codebase that imports LLVM headers. Both the JIT backend and
the AOT backend consume the output of this stage.

`IRLowering` walks the AST top-down and emits LLVM IR instructions into
an `llvm::Module`. The mapping is direct and deterministic — every Lucid
construct has exactly one IR pattern:

| Lucid construct                 | LLVM IR output                  |
| ------------------------------- | ------------------------------- |
| `const f (x T) -> R`            | `define R @f(T %x)`             |
| `let x T = v`                   | `alloca T` + `store`            |
| `@[foreign("C")] const g (...)` | `declare` (unresolved symbol)   |
| `#sqrt(x)`                      | `call @llvm.sqrt.f32(float %x)` |
| `#memcpy(d, s, n)`              | `call @llvm.memcpy(...)`        |
| `#toRef(p)`                     | non-null assertion + `bitcast`  |
| `*T` (raw pointer)              | `ptr` (LLVM opaque pointer)     |
| `Result<T, Flag>`               | `{ T, i1 }` (LLVM struct type)  |
| `async f(args)`                 | LLVM coroutine intrinsics       |

`TypeMapping` handles the Lucid-type → LLVM-type conversion. It is
used by both `IRLowering` and the `FFIValidator`.

`ForeignDecl` handles the special case of `@[foreign("C")]` declarations:
it emits a `declare` statement naming the external C symbol. The JIT resolves
this at runtime via `dlopen`; the AOT linker resolves it at link time. Neither
path needs `libffi` — LLVM's own codegen handles calling conventions.

**Key files:**
- `IRLowering.hpp/cpp` — AST → LLVM IR; main entry point
- `TypeMapping.hpp/cpp` — Lucid types → LLVM types
- `Intrinsics.hpp/cpp` — `#intrinsic` → `llvm.*` mappings
- `ForeignDecl.hpp/cpp` — `@[foreign("C")]` → `declare` + `call`

---

## 5. Backend — Interpreter (ORC JIT)

**Location:** `src/interpreter/`

The interpreter backend receives the LLVM IR module from `IRLowering` and
executes it immediately using LLVM's ORC (On-Request Compilation) JIT
framework. No object file is written to disk.

### JIT Session Setup

`JIT` creates an `llvm::orc::LLJIT` instance backed by the host machine's
native target. The IR module is added to the JIT session as a `ThreadSafeModule`.
The JIT compiles it to native machine code in memory and makes it available
for execution.

### Foreign Symbol Resolution

For every `@[link("libname")]` annotation in the module, `DynLink` calls
`dlopen("libname.so")` on Linux/macOS or `LoadLibrary("libname.dll")` on
Windows. All exported symbols from the loaded library are registered with
the JIT's `DynamicLibrarySearchGenerator`. When the JIT-compiled code calls
a foreign function, the symbol is resolved from this table — a direct
function call, no marshaling layer.

`luc_kernel.dll` is always loaded first, before any user module. Its symbols
cover the entire `lge_*.h` API surface. Other libraries named in `@[link]`
are loaded on demand.

### Hot-Reload

When the file watcher (in `runtime/`) detects a source file change, the
interpreter:
1. Re-runs lexer → parser → sema → IR lowering on the changed module
2. Adds the new IR module to the existing JIT session under a new version key
3. Removes the old module version from the session
4. The next call to any function in that module resolves to the new version

Because all game state lives in `luc_kernel`'s ECS (not inside Lucid
functions), the hot-swapped module picks up exactly where the old one left off.

**Key files:**
- `JIT.hpp/cpp` — LLVM ORC JIT session: setup, module loading, symbol lookup,
  hot-reload swap
- `DynLink.hpp/cpp` — platform wrapper for `dlopen` / `LoadLibrary`;
  registers library symbols with the JIT's search generator

---

## 6. Backend — Compiler (AOT)

**Location:** `src/compiler/aot/`

The AOT backend receives the same LLVM IR module and produces a native
binary or shared library via the system linker. It runs the full LLVM
optimisation pipeline before emitting.

`AOT` sets up an `llvm::TargetMachine` for the target platform, runs
`llvm::PassManager` with the requested optimisation level (`-O0` through
`-O3`), and writes the object file via `llvm::raw_fd_ostream`.

`Linker` collects all `@[link("libname")]` annotations from the module and
assembles a linker invocation. On Linux: `ld` or `lld` with `-lname` flags.
On Windows: `link.exe` with `/lib:name.lib`. The result is a native `.exe`,
`.dll`, or `.so` depending on the target.

**Key files:**
- `aot/AOT.hpp/cpp` — `TargetMachine` setup, optimisation pipeline, object
  file emission
- `aot/Linker.hpp/cpp` — assembles and invokes the system linker from
  `@[link]` annotations

---

## 7. Runtime

**Location:** `src/runtime/`

The runtime provides support services that execute alongside the compiled or
JIT-executed Lucid program. Unlike the frontend and backends, the runtime is
linked into the final binary and runs at program execution time.

### Memory (`memory.hpp/cpp`)

Implements the Lucid memory model:
- The allocation registry hash map backing `#alloc` / `#free` — tracks all
  live heap allocations, catches double-free and use-after-free at the Lucid
  level
- `ArenaDescriptor` creation and bump-pointer management for `#arena_create`
  / `#arena_alloc` / `#arena_reset` / `#arena_free`
- The internal allocation cursor for named arenas (not exposed in
  `ArenaDescriptor.base` / `size` — those are the stable boundary fields
  shared with C)

### Threading (`threading.hpp/cpp`)

Implements `async`, `parallel`, `await`, and `join`:
- `async f(args)` — submits `f` to a thread pool, returns a future handle
- `parallel f(args)` — spawns a detached thread, no return value
- `await handle` — blocks the calling fiber/thread until the future resolves
- `join handle` — same as await but for parallel threads

Built on top of the platform thread primitives from `lge_platform.h` in the
kernel. The thread pool is shared with the engine's job system.

### FFI (`ffi/`)

The FFI layer manages the boundary between Lucid and C at runtime:
- `FFI.hpp/cpp` — entry point; dispatches foreign calls in tree-walk mode
  (unused in ORC JIT path where LLVM handles this directly)
- `DynLink.hpp/cpp` — `dlopen` / `LoadLibrary` wrapper shared with the
  interpreter backend
- `TypeMarshal.hpp/cpp` — Lucid value layout ↔ C ABI layout conversion
  (used only in the tree-walk path if it exists; not needed for ORC JIT)

---

## 8. Standard Library

**Location:** `src/stdlib/`

The standard library is written in Lucid, not C++. Each module is a `.luc`
file that the Compiler compiles alongside user code. Standard library
modules are resolved before user modules in the dependency order so their
declarations are always available.

| Module       | Provides                                              |
| ------------ | ----------------------------------------------------- |
| `io.luc`     | Console I/O, file reading/writing via the VFS         |
| `math.luc`   | Arithmetic utilities, trigonometry, random            |
| `array.luc`  | Array operations: map, filter, reduce, sort, zip      |
| `string.luc` | String manipulation: split, trim, find, format        |
| `http.luc`   | Basic HTTP client (future)                            |
| `game.luc`   | Game-specific helpers: Vec2/Vec3 math, entity helpers |

Standard library modules use `@[foreign("C")]` declarations internally to
call into the kernel's C API. They present clean Lucid APIs to game developers
who never need to see the C boundary.

---

## 9. CLI and LSP

### CLI (`src/cli/`)

The `lucid` binary exposes three commands:

```
lucid run   <file.luc>          -- JIT interpret and execute
lucid build <file.luc> -o out   -- AOT compile to native binary
lucid repl                        -- interactive REPL (run mode per line)
```

Each command is a thin wrapper that drives the shared pipeline:
`commands.cpp` dispatches to `run.hpp`, `build.hpp`, or `repl.hpp`.

### LSP (`src/lsp/`)

The server implements the Server Protocol so editors
(VS Code, Neovim, etc.) can provide:
- Real-time diagnostics (errors and warnings as you type)
- Autocomplete for identifiers, fields, and type annotations
- Go-to-definition and find-references
- Hover documentation from doc-comments

The LSP server reuses the lexer, parser, and semantic analysis passes on each
file change, running them incrementally where possible. It runs as a separate
process and communicates with the editor via stdin/stdout JSON-RPC.

---

## 10. File Structure

```
lucid/
├── README.md
├── LICENSE
├── CMakeLists.txt
├── .gitignore
│
├── docs/
│   ├── LUCID_GRAMMAR.md          -- grammar reference
│   ├── ARCHITECTURE.md           -- this document
│   ├── API.md                    -- standard library API
│   ├── BUILD.md                  -- how to build Lucid
│   └── examples/                 -- example .luc files
│
└── src/
    ├── main.cpp                  -- CLI entry point (lucid run / build / repl)
    │
    ├── core/                     -- shared data structures (no LLVM dependency)
    │   ├── Tokens.hpp            -- Token and TokenKind definitions
    │   ├── diagnostics/          -- user-facing error and warning reporting
    │   ├── ast/                  -- AST node types
    │   │   ├── BaseAST.hpp       -- base node, SourceLocation, visitor interface
    │   │   ├── DeclAST.hpp       -- declaration nodes
    │   │   ├── StmtAST.hpp       -- statement nodes
    │   │   ├── ExprAST.hpp       -- expression nodes
    │   │   └── TypeAST.hpp       -- type annotation nodes
    │   └── memory/               -- AST memory management
    │       ├── ArenaSpawn.hpp    -- arena allocator for AST nodes
    │       ├── ASTArena.hpp      -- arena instance holding one module's nodes
    │       ├── InternedString.hpp
    │       └── StringPool.hpp/cpp
    │
    ├── parser/                   -- frontend stage 1: source → AST
    │   ├── Parser.hpp            -- public interface: parse()
    │   ├── Parser.cpp
    │   ├── ModuleResolver.hpp/cpp -- multi-file resolution, cyclic import detection
    │   ├── lexer/
    │   │   └── Lexer.hpp/cpp           -- character stream → token stream
    │   ├── rules/                      -- grammar rule implementations
    │   │   ├── ParserDecl.cpp          -- const, let, struct, enum, trait, fn
    │   │   ├── ParserStmt.cpp          -- if, for, while, return, block
    │   │   ├── ParserExpr.cpp          -- Pratt parser: all expressions
    │   │   ├── ParserType.cpp          -- type annotations: *T, T?, generics
    │   │   └── Concurrency.cpp   -- async, parallel, await, spawn, join
    │   └── support/                    -- parser infrastructure
    │       ├── ParserContext.hpp       -- shared parse state
    │       ├── Lookahead.cpp           -- disambiguation helpers
    │       ├── Helpers.cpp             -- attribute parsing, doc-comment handling
    │       ├── TokenStream.hpp/cpp     -- Track stream of tokens when parsing a file
    │       └── ErrorRecovery.cpp       -- sync points for error recovery
    │
    ├── sema/                     -- frontend stage 2: AST → validated AST
    │   ├── Sema.hpp/cpp           -- public interface: Sema::analyze()/analyzeAll(), analyzeModuleDecls()
    │   ├── SemaContext.hpp        -- shared state for all sema passes
    │   ├── rules/                        -- single-traversal analysis: name resolution + type
    │   │   │                                checking happen together (see Sema.hpp's "One
    │   │   │                                traversal, not two" note), not as separate passes
    │   │   ├── SemaDecl.cpp              -- const, let, struct, enum, trait, fn, fields, params
    │   │   ├── SemaStmt.cpp              -- if, for, while, switch, return, block
    │   │   ├── SemaExpr.cpp              -- literals, binary/unary, calls, pipeline, compose
    │   │   ├── SemaType.cpp              -- resolve named/array/nullable/fallible/ptr/ref/func types
    │   │   ├── Generics.cpp              -- generic param usage, trait implementation, self-reference
    │   │   ├── Concurrency.cpp           -- async, await, spawn, join
    │   │   └── FFIValidator.hpp/cpp      -- validate @[foreign("C")] against lge_ffi.lfi
    │   └── support/                      -- sema infrastructure
    │       ├── Resolution.cpp            -- resolveValueOrError/resolveTypeNameOrError/
    │       │                                resolveCalleeOrError/selfTypeOf
    │       ├── TypeCompat.cpp            -- typesEqual/isAssignable/nullable-fallible helpers
    │       └── Attributes.cpp            -- validateAttributes/validateAttribute
    │
    ├── codegen/
    │   ├── IRLowering.hpp                     # Single unified header (all declarations)
    │   ├── IRLowering.cpp                     # Main entry point + orchestration
    │   ├── IRLoweringDecl.cpp                 # Declaration lowering (functions, vars, structs, enums)
    │   ├── IRLoweringStmt.cpp                 # Statement lowering (if, for, while, return, etc.)
    │   ├── IRLoweringExpr.cpp                 # Expression lowering (literals, binary, calls, etc.)
    │   ├── IRLoweringIntrinsic.cpp            # Intrinsic lowering (#sqrt, #memcpy, #ptrDiff, etc.)
    │   ├── IRLoweringBuilder.cpp              # Helper builders for common IR patterns
    │   ├── TypeMapping.hpp/cpp                # Lucid → LLVM type mapping (stays single file)
    │   └── IntrinsicRegistry.hpp/cpp          # Maps Lucid intrinsic names → LLVM IDs
    │
    ├── interpreter/              -- ORC JIT backend (lucid run)
    │   ├── Interpreter.hpp/cpp   -- Main interpreter engine
    │   ├── JIT.hpp/cpp           -- LLVM ORC JIT session
    │   └── DynLink.hpp/cpp       -- dlopen/LoadLibrary
    │
    ├── compiler/                 -- AOT backend (lucid build)
    │   └── aot/                  -- AOT-only backend
    │       ├── AOT.hpp/cpp       -- optimisation pipeline + object file emission
    │       └── Linker.hpp/cpp    -- system linker invocation
    │
    ├── runtime/                  -- services that run alongside the program
    │   ├── memory.hpp/cpp        -- #alloc/#free registry, ArenaDescriptor management
    │   ├── threading.hpp/cpp     -- async/parallel/await/join implementation
    │   └── ffi/
    │       ├── FFI.hpp/cpp       -- foreign call dispatch
    │       ├── DynLink.hpp/cpp   -- shared dlopen wrapper (used by runtime + JIT)
    │       └── TypeMarshal.hpp/cpp -- Lucid ↔ C type layout (fallback path only)
    │
    ├── stdlib/                   -- standard library (written in Lucid)
    │   ├── io.luc
    │   ├── math.luc
    │   ├── array.luc
    │   ├── string.luc
    │   ├── http.luc
    │   └── game.luc
    │
    ├── cli/                      -- command-line interface
    │   ├── commands.hpp/cpp      -- command dispatch
    │   ├── run.hpp               -- lucid run
    │   ├── build.hpp             -- lucid build
    │   └── repl.hpp              -- lucid repl
    │
    ├── lsp/                      -- Server Protocol
    │   ├── server.hpp/cpp        -- JSON-RPC server, incremental re-analysis
    │   └── handlers.hpp          -- LSP request handlers (hover, complete, goto)
    │
    └── debug/                    -- developer tools (not user-facing)

tests/
├── parser/
│   ├── test_lexer.cpp
│   └── test_parser.cpp
├── sema/
│   ├── test_decl.cpp
│   ├── test_stmt.cpp
│   ├── test_expr.cpp
│   ├── test_type.cpp
│   ├── test_generics.cpp
│   └── test_ffi_validator.cpp
├── interpreter/
│   └── test_jit.cpp
├── compiler/
│   ├── test_ir_lowering.cpp
│   └── test_aot.cpp
├── runtime/
│   ├── test_memory.cpp
│   └── test_threading.cpp
├── stdlib/
│   ├── test_io.luc
│   └── test_math.luc
└── integration/
    └── test_games.luc
```