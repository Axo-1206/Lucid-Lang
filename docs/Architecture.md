# Lucid Compiler — Architecture

> This document describes the internal architecture of the Lucid
> Compiler: how the source code flows from text to execution, how the major
> subsystems are structured, and how each folder in the codebase fits into
> that flow.
>
> llvm version: 18.1.6
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
  - [7. Runtime (outdated, need an update)](#7-runtime-outdated-need-an-update)
    - [Memory (`memory.hpp/cpp`)](#memory-memoryhppcpp)
    - [Threading (`threading.hpp/cpp`)](#threading-threadinghppcpp)
    - [FFI (`ffi/`)](#ffi-ffi)
  - [8. Standard Library](#8-standard-library)
  - [9. Distribution — File Extensions, Libraries, and Module Merging](#9-distribution--file-extensions-libraries-and-module-merging)
    - [9.1 File Extensions](#91-file-extensions)
    - [9.2 `.luci` — the Library Interface File](#92-luci--the-library-interface-file)
    - [9.3 Static vs. Shared Libraries](#93-static-vs-shared-libraries)
    - [9.4 `.bc` — Portable Bitcode Distribution](#94-bc--portable-bitcode-distribution)
    - [9.5 Whole-Program Module Merging](#95-whole-program-module-merging)
    - [9.6 Calling Another Lucid Library at Runtime](#96-calling-another-lucid-library-at-runtime)
  - [10. CLI and LSP](#10-cli-and-lsp)
    - [CLI (`src/cli/`)](#cli-srccli)
    - [LSP (`src/lsp/`)](#lsp-srclsp)
  - [11. File Structure](#11-file-structure)

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
                    │   (.luc)        │
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
   ┌──────────▼──────────┐     ┌────────────▼─────────────┐
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

**`ModuleEmitOptions` — the one narrow input `build` needs here.** IR
Lowering stays identical between `run` and `build` for every Lucid
construct in the table above. The only per-build-mode input it takes is a
small struct (`{ OutputKind kind; TargetOS targetOS; }`, `OutputKind` being
`Executable`/`StaticLib`/`SharedLib`) consulted at exactly two points, never
branching the AST walk itself:
- whether an `@[export] const main` entry is required (`Executable`) or
  disallowed (`StaticLib`/`SharedLib`),
- whether an `@[export]`ed function needs `dllexport` linkage — only
  relevant for `SharedLib` targeting Windows.

Everything else that distinguishes a static archive from a shared library
from an executable happens downstream, after `IRLowering` has already
produced its one unchanged `llvm::Module` — see **Distribution**, below.

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

`IRLowering` still produces one `llvm::Module` per source file, the same as
the JIT path — but unlike the JIT (which keeps modules separate so
hot-reload has something granular to swap), `AOT` first merges every
per-file module into a single whole-program `llvm::Module` via
`llvm::Linker::linkModules`, before running any optimisation pass. This
unlocks cross-file inlining and whole-program dead-code elimination that
per-file boundaries would otherwise block (a `declare`d cross-file call
can't be inlined; an ordinary in-module call can). See **Distribution §9.5**
for the full rationale.

`AOT` sets up an `llvm::TargetMachine` for the target platform — selecting
`Reloc::PIC_` when `ModuleEmitOptions.kind == SharedLib`, the platform
default otherwise, since position-independent codegen is a `TargetMachine`
concern, not something `IRLowering` needs to know about — runs
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

## 7. Runtime (outdated, need an update)

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

## 9. Distribution — File Extensions, Libraries, and Module Merging

This section covers what happens once a Lucid file needs to leave the
project it was written in — as a library another project links against, or
as a portable compiled artifact handed to someone without the `.luc`
source. None of this changes the frontend or `IRLowering`; it's entirely
about what happens to the `llvm::Module`(s) they produce, downstream.

### 9.1 File Extensions

| Extension                         | What it is                                                                                             | Produced by                                         | Consumed by                                                                                                                                                                           |
| --------------------------------- | ------------------------------------------------------------------------------------------------------ | --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `.luc`                            | Lucid source                                                                                           | the developer                                       | frontend (lexer → parser → sema)                                                                                                                                                      |
| `.o`                              | Object file — one translation unit's machine code, unlinked                                            | AOT backend, internal intermediate                  | system linker only; not a first-class CLI output                                                                                                                                      |
| `.a`                              | Static library archive                                                                                 | `lucid build --lib` (default / `--static`)          | system linker only, at build time. Cannot be `dlopen`'d — code is copied into the consumer at link time                                                                               |
| `.so` / `.dylib` / `.dll`         | Shared/dynamic library                                                                                 | `lucid build --lib --shared`                        | system linker (if linked against at build time) **or** dynamic linker (`dlopen`/`LoadLibrary`) at run time — either `DynLink` at JIT startup, or a user's own `dynlib:load(...)` call |
| `.lib` (Windows only)             | Import stub for a `.dll`                                                                               | alongside `.dll`, on Windows shared builds          | system linker (`link.exe`) only — never loaded at runtime itself                                                                                                                      |
| `.luci`                           | Lucid interface file — exported signatures, types, struct layouts, doc comments; no bodies             | `lucid build --lib` (any static/shared combination) | Sema's `import` resolution, and the LSP (one format, two consumers — see §9.2)                                                                                                        |
| `.bc`                             | LLVM bitcode — a serialized `llvm::Module`, target-agnostic, frozen before any target-specific codegen | `lucid build --emit-bc` (see §9.4)                  | `lucid run <file>.bc` — same `JITSession`/`JITCompiler` path as source, just skipping lexer/parser/sema/`IRLowering`                                                                  |
| `.lfi` (existing — `lge_ffi.lfi`) | C-side symbol table for `@[foreign("C")]` validation                                                   | shipped with the Engine SDK                         | `FFIValidator` only, at Sema time — describes *C* symbols Lucid calls into, the mirror image of `.luci`                                                                               |

### 9.2 `.luci` — the Library Interface File

Compiled machine code carries no types — a `.o`/`.a`/`.so` gives you a
symbol table at best, never parameter types, struct layouts, or which
symbols are meant to be called externally versus internal names that
happen to leak. `.luci` is the Lucid-native equivalent of a C header,
generated by the compiler instead of hand-written: a serialized projection
of the typed AST's exported subset (everything the module system already
tracks via `@[export]`), including doc comments (per **Doc Comment
Attachment Rules** in the grammar), so hover/autocomplete work without
source.

Both of `.luci`'s consumers read the exact same file, deliberately — the
same design constraint as TypeScript's `.d.ts`, which serves `tsc` and
every editor's language server identically. A new Sema pass, `LibValidator`
(parallel in shape to the existing `FFIValidator`, just checking against
`.luci` instead of `lge_ffi.lfi`), validates `import lib;` call sites
against it; `luc_langserver` reads it for autocomplete/hover on an
imported library whose source isn't present. Neither ever inspects the
compiled binary itself.

### 9.3 Static vs. Shared Libraries

`--lib` alone determines *what's exported*; `--static`/`--shared` is a
separate, orthogonal choice about *how it's linked* — and it isn't free to
produce both, so it isn't the default:

- **`.so`/`.dylib`/`.dll` require position-independent codegen (PIC)**,
  set via `Reloc::PIC_` on the `TargetMachine` (see §6). `.a` typically
  doesn't need it. Producing both means a second codegen pass through
  `AOT`, not just re-archiving the same `.o`s.
- **Default is `--static`.** Case A below (importing a known library at
  compile time) is the overwhelmingly common case, and static is cheaper.
  Shared is opt-in, the same way every other more-powerful, more-dangerous
  capability in this design is opt-in rather than default.
- **Generics crossing this boundary use Option B: closed, pre-instantiated
  concrete types only** (`extern template`-style), not "ship the generic
  body and let the consumer instantiate" (what Rust's `.rlib` metadata
  does). This keeps the library's ABI stable across compiler versions —
  `lib.a`/`lib.so` is plain machine code with concrete symbols, same
  guarantee C libraries have always had. A library author who wants
  `Array<T>` usable across the boundary picks the concrete `T`s they
  support (`Array<int>`, `Array<Player>`, ...) and only those show up in
  `.luci` — a consumer requesting an unlisted `T` gets a compile error,
  the same as it would trying to use a type `libfoo.a` never anticipated
  in C.

| Flag                                          | Produces                                                                                             |
| --------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| `lucid build --lib lib.luc`                   | `lib.a` + `lib.luci` (default)                                                                       |
| `lucid build --lib --static lib.luc`          | `lib.a` + `lib.luci` (explicit spelling of the default)                                              |
| `lucid build --lib --shared lib.luc`          | `lib.so`/`.dylib`/`.dll` (+ `.lib` stub on Windows) + `lib.luci`                                     |
| `lucid build --lib --static --shared lib.luc` | all of the above, one shared `.luci` — only when genuinely needed, accepting the double-codegen cost |

### 9.4 `.bc` — Portable Bitcode Distribution

`.bc` is not a new execution engine — it's `llvm::BitcodeWriter` serializing
the exact same `llvm::Module` type `IRLowering` already produces, frozen at
the point *before* either backend makes any target-specific decision
(instruction selection, register allocation). That's what makes it
portable: nothing CPU-specific has happened to it yet.

The usual objection to distributing raw LLVM bitcode — that `.bc` isn't
guaranteed stable across LLVM versions — doesn't apply here, because LLVM
is statically bundled into `lucid` at a single pinned version (see the
size-budget note in §1); the producer and the consumer of any `.bc` Lucid
ever writes are always the same LLVM version, by construction.

```
lucid build --emit-bc main.luc   -- target-independent opt passes, then
                                     BitcodeWriter → main.bc (no target
                                     codegen — still portable)

lucid run main.bc                -- BitcodeReader reconstructs the Module,
                                     skips lexer/parser/sema/IRLowering
                                     entirely, hands it to the same
                                     JITSession used for source
```

Running LLVM's target-independent optimisation passes (dead code
elimination, inlining, constant folding — the parts of `-O2`/`-O3` that
don't depend on a specific CPU) before writing `.bc` means every
recipient's JIT only has to pay for fast, target-specific codegen at load
time, not the expensive analysis work too — that was already paid for once
by whoever built the `.bc`.

This gives Lucid a third distribution tier, alongside source and a fully
standalone AOT binary:

| Tier              | Command                | Needs on target machine |
| ----------------- | ---------------------- | ----------------------- |
| Source            | `lucid run main.luc`   | `lucid` binary          |
| Portable compiled | `lucid run main.bc`    | `lucid` binary          |
| Standalone native | `lucid build main.luc` | nothing                 |

### 9.5 Whole-Program Module Merging

`IRLowering` always produces one `llvm::Module` per file — that part is
identical everywhere. What differs is whether anything merges them
afterward, and it's a deliberate split, not an oversight:

- **`lucid run` keeps modules separate**, one per file, each added to the
  `JITSession` individually. This is the unit hot-reload swaps — see §5,
  "Hot-Reload" — and merging would remove the granularity that mechanism
  depends on. The cost: a call across a file boundary is a `declare`
  resolved through the JIT's symbol table, which LLVM cannot inline
  through. Acceptable here, since fast iteration is the actual goal, not
  peak throughput, and `run` already favors low optimisation levels.
- **`lucid build` and `lucid build --emit-bc` merge first**, via
  `llvm::Linker::linkModules`, into one whole-program `Module`, before
  optimisation or codegen. Nothing swaps a function mid-execution in a
  shipped binary or bitcode file, so there's no reason to keep the
  fragmentation — merging unlocks cross-file inlining and true
  whole-program dead-code elimination (a private helper unused by the
  *program*, not just unused within its own file, can now be proven dead
  and stripped), which also produces a smaller, denser `.bc`.

### 9.6 Calling Another Lucid Library at Runtime

Two distinct cases, handled by different mechanisms:

**Case A — `import lib;`, known at compile time.** Not a new mechanism:
`IRLowering` emits a `declare` for each imported symbol exactly as it does
for `@[foreign("C")]`; at JIT time, `DynLink` (§5) `dlopen`s the library
and registers its symbols with the JIT's `DynamicLibrarySearchGenerator`,
same as any `@[link(...)]` target; at AOT time, `Linker` (§6) passes it as
a normal `-l` flag. The only new work is `LibValidator` (§9.2) checking the
call site against `.luci` instead of `lge_ffi.lfi` — everything downstream
of Sema is unchanged.

**Case B — genuinely dynamic (plugin-style) loading, where the library's
identity isn't known until runtime.** No `.luci` can be checked against,
because which file to load is itself a runtime value — this needs an
explicit, visible, unsafe-flavored API, the same idiom C's `dlsym` uses:
`dynlib:load(path) -> DynLibrary!` / `dynlib:symbol(handle, name) -> *void?`,
with the caller responsible for `#bitcast`ing the result to whatever
function-pointer type they believe is correct (see **Alternatives to
Type-Erased Generics**, case 3, in the grammar reference — this is the same
escape hatch). Implementation-wise, this is a thin `extern "C"` pair
(`__lucid_dynlib_load` / `__lucid_dynlib_symbol`) wrapping the *existing*
`interpreter/dynlink/DynamicLinker` + `LibraryHandle` machinery, exposed as
a `stdlib/dynlib.luc` module the same way `io.luc`/`math.luc` wrap their
own runtime entry points.

**This needs to work under AOT too, which is a real gap today.**
`compiler/aot/Linker.hpp/cpp` currently only does compile-time linking —
it has no runtime `dlopen` capability at all, because nothing has needed
one before. For `dynlib:load(...)` to work inside a `lucid build`-produced
binary (not just under `lucid run`, where the JIT's `DynLink` already
exists), the `__lucid_dynlib_*` entry points need to be part of the Lucid
runtime statically linked into every AOT binary — a sibling to
`MemoryRuntime`/`ConcurrencyRuntime` under `codegen/runtime/`, not
something reachable only through the interpreter. Worth testing under AOT
specifically once built, not just assumed to follow from the JIT path
working.

---

## 10. CLI and LSP

### CLI (`src/cli/`)

Each command is a thin wrapper that drives the shared pipeline: `main.cpp`
dispatches to the frontend commands (`run.hpp`, `cli/frontend/parse.hpp`,
`cli/frontend/sema.hpp`) or the AOT backend (`build.hpp`). `parse` and
`sema` exist to stop the pipeline early, at the `Parse` and `Sema` stages
respectively (see **Execution Pipeline**), for debugging and tooling —
they don't produce an executable or library artifact.

**Target command surface** (this is the authoritative reference; see the
note below on `main.cpp`'s current state relative to it):

```
lucid run   <file.luc>                      -- JIT interpret and execute; no file output
lucid run   <file.bc>                       -- JIT-execute portable bitcode (§9.4);
                                                skips lexer/parser/sema/IRLowering
lucid parse <file.luc> [--json|--json-pretty] [-o out]   -- stop after AST
lucid sema  <file.luc> [--json|--json-pretty] [-o out]   -- stop after semantic analysis
lucid build <file.luc> [-o out] [-O<0-3>] [--target <triple>]
                                             -- AOT compile to a native executable
lucid build --lib <file.luc> [--static] [--shared] [-O<0-3>] [--target <triple>]
                                             -- produce a library: .a and/or .so/.dylib/.dll,
                                                always alongside a .luci (§9.1–9.3)
lucid build --emit-bc <file.luc> [-o out.bc] -- produce portable bitcode (§9.4)
lucid repl                                  -- interactive REPL (run mode per line)
```

Flags relevant to `run` (`--verbose`, `--trace`, `--no-hot-reload`,
`-O<level>`, `--entry <name>`) apply unchanged to both the `.luc` and
`.bc` forms of `run`.

> [!NOTE]
> **Current implementation status.** `main.cpp` today implements `run`,
> `parse`, and `sema` against `CLIOptions`; `build` is recognized but not
> yet implemented (`repl` likewise). It does not yet know about
> `--lib`, `--static`, `--shared`, `--emit-bc`, or `ModuleEmitOptions` —
> those are captured here as the target design this document describes,
> not as already-wired CLI flags. `main.cpp` is expected to be refactored
> to match this section once `build` is implemented; until then, this
> table is the source of truth for what the CLI *should* accept, and
> `main.cpp`'s own header comment reflects only what it *currently*
> accepts.

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

## 11. File Structure

```
lucid/
├── README.md
├── LICENSE
├── CMakeLists.txt
├── .gitignore
├── .patches/ # contain patches from AI agent, use the Editor command to automatically apply new patches instead of manual edit
│
├── docs/
│   ├── internal/                           # compiler note and architecture
│   ├── LUCID_GRAMMAR.md                    # grammar reference
│   ├── ARCHITECTURE.md                     # this document
│   ├── API.md                              # standard library API
│   ├── BUILD.md                            # how to build Lucid
│   └── examples/                           # example .luc files
│
├── temp/   # temporary files
└── src/
    ├── main.cpp                            # CLI entry point (lucid run / build / repl)
    │
    ├── core/                               # shared data structures (no LLVM dependency)
    │   ├── ASTStrings.hpp                  # Convert LLVM values and AST nodes into strings
    │   ├── JSONFormatter.hpp/cpp           # Format parse and sema output as JSON for tooling/LSP
    │   ├── SourceLocation.hpp              # Definition of AST node source location
    │   ├── Tokens.hpp                      # Token and TokenKind definitions
    │   │
    │   ├── ast/                            # AST node types
    │   │   ├── BaseAST.hpp                 # Base AST node, SourceLocation, visitor interface
    │   │   ├── DeclAST.hpp                 # Declaration nodes
    │   │   ├── ExprAST.hpp                 # Expression nodes
    │   │   ├── ResourceKind.hpp/cpp        # ResourceKind classification of a Lucid type
    │   │   ├── StmtAST.hpp                 # Statement nodes
    │   │   └── TypeAST.hpp                 # Type annotation nodes
    │   │
    │   ├── builtins/
    │   │   └── ArenaMethod.hpp             # Built-in arena method enum and parsing
    │   │
    │   ├── diagnostics/                    # User-facing error and warning reporting
    │   │   ├── Diagcode.hpp                # Diagnostic error codes
    │   │   ├── Diagnostic.hpp/cpp          # Diagnostic engine & reporting
    │   │   └── StackTrace.hpp/cpp          # Stack trace capture & formatting
    │   │
    │   ├── memory/                         # AST memory management
    │   │   ├── ArenaSpan.hpp               # Custom arena-allocated span helper
    │   │   ├── ASTArena.hpp                # Arena instance holding AST nodes
    │   │   ├── InternedString.hpp          # Interned string reference
    │   │   └── StringPool.hpp/cpp          # String interning pool
    │   │
    │   ├── registry/
    │   │   ├── AttributeRegistry.hpp/cpp   # Compiler attribute registration
    │   │   └── IntrinsicRegistry.hpp/cpp   # Compiler intrinsic registration
    │   │
    │   └── trace/
    │       └── Trace.hpp/cpp               # Execution tracing utility
    │
    ├── parser/                             # frontend stage 1: source → AST
    │   ├── Parser.hpp/cpp                  # Public parser entry points
    │   ├── ModuleResolver.hpp/cpp          # Multi-file module resolution & cyclic import detection
    │   │
    │   ├── context/
    │   │   ├── ParserContext.hpp           # Sync points for error recovery and parser state
    │   │   └── TokenStream.hpp/cpp         # Token stream management and traversal
    │   │
    │   ├── lexer/
    │   │   └── Lexer.hpp/cpp               # Character stream → token stream
    │   │
    │   ├── rules/                          # Grammar rule implementations
    │   │   ├── ParseDecl.cpp               # Declarations (const, let, struct, enum, trait, fn)
    │   │   ├── ParseExpr.cpp               # Pratt parser: all expression rules
    │   │   ├── ParseStmt.cpp               # Statements (if, for, while, return, block)
    │   │   └── ParseType.cpp               # Type annotations (*T, T?, generics)
    │   │
    │   └── support/                        # Parser infrastructure helpers
    │       ├── ErrorRecovery.hpp/cpp       # Parser error recovery state & routines
    │       ├── Helpers.cpp                 # Attribute parsing, doc-comments, general helpers
    │       └── LookAhead.cpp               # Disambiguation and lookahead helpers
    │
    ├── sema/                               # frontend stage 2: semantic analysis
    │   ├── Sema.hpp/cpp                    # Public API (namespace sema)
    │   │
    │   ├── const_eval/                     # Compile-time evaluation
    │   │   ├── ConstEvaluator.hpp/cpp      # Main evaluator interface & orchestration
    │   │   ├── ConstEvalBinary.cpp         # Binary operations evaluation
    │   │   ├── ConstEvalHelpers.hpp/cpp    # Helper declarations & utilities
    │   │   ├── ConstEvalStatement.cpp      # Statement execution evaluation
    │   │   └── ConstEvalUnary.cpp          # Unary operations evaluation
    │   │
    │   ├── context/                        # Context & generic instantiation
    │   │   ├── ContextStack.hpp/cpp        # Scope/context stack management
    │   │   ├── Generic.hpp/cpp             # Generic substitution and instantiation utilities
    │   │   ├── Instantiation.cpp           # Generic instantiation orchestration & caching
    │   │   └── SemaContext.hpp/cpp         # Unified semantic context composition
    │   │
    │   ├── registry/
    │   │   ├── ArgTypeValidators.hpp/cpp   # Argument type validators for attributes/intrinsics
    │   │   ├── AttributeValidator.hpp/cpp  # Attribute existence & argument validator
    │   │   └── IntrinsicValidator.hpp/cpp  # Intrinsic existence & argument validator
    │   │
    │   ├── rules/                          # Semantic analysis rules
    │   │   ├── SemaDecl.cpp                # Declaration semantic checks
    │   │   ├── SemaExpr.cpp                # Expression semantic checks
    │   │   └── SemaStmt.cpp                # Statement semantic checks
    │   │
    │   ├── support/
    │   │   ├── CaptureAnalysis.hpp/cpp     # Closure capture analysis
    │   │   ├── MangledName.hpp/cpp         # Symbol name mangling helpers
    │   │   ├── SwitchHelpers.hpp/cpp       # Switch statement semantic validation helpers
    │   │   ├── Truthiness.hpp              # Truthiness evaluation rules
    │   │   └── TypeNarrowHelpers.hpp/cpp   # Control-flow type narrowing helpers
    │   │
    │   └── types/                          # Type system checking & resolution
    │       ├── SemaResolve.cpp             # Type annotation resolution to semantic types
    │       ├── SemaType.hpp                # Main header for semantic types
    │       ├── SemaTypeEquality.cpp        # Type equality checking
    │       ├── SemaTypePredicates.cpp      # Type classification predicates
    │       └── SemaValidate.cpp            # Type validation helpers
    │
    ├── runtime-abi/
    │   ├── functions.def                   # The single source of truth for runtime ABI functions
    │   ├── lucid_abi.h                     # ABI contract between CodeGen, runtime, and interpreter
    │   └── lucid_runtime.h                 # extern "C" prototype of every runtime function, expanded from functions.def
    │
    ├── runtime/                            # Lucid native runtime support library
    │   ├── ArenaRuntime.cpp                # Implementation of arena runtime functions
    │   ├── ClosureEnvironment.hpp          # Closure environment memory layout & management
    │   ├── ClosureRuntime.cpp              # Extern "C" entry points for closure runtime
    │   ├── ConcurrencyEntry.cpp            # Extern "C" entry points for concurrency runtime
    │   ├── ConcurrencyRuntime.hpp/cpp      # Thread pool, event loop, registry
    │   ├── exports.cpp                     # Takes address of every runtime function; linker enforces completeness
    │   ├── MemoryRuntime.cpp               # Memory management runtime functions
    │   ├── PanicRuntime.cpp                # Panic implementation
    │   ├── RuntimeError.hpp                # Runtime error definitions
    │   ├── RuntimeInternal.hpp             # Internal runtime helpers shared across runtime translation units
    │   └── StringRuntime.cpp               # String operations runtime
    │
    ├── codegen/                            # LLVM IR code generator
    │   ├── Abi.hpp/cpp                     # Typed, cached access to the runtime ABI surface
    │   ├── CodeGen.hpp/cpp                 # The one public entry point of the codegen subsystem
    │   ├── FailureKind.hpp/cpp             # Closed set of runtime failures the compiler emits, and the isRiskyLhs predicate
    │   ├── FunctionState.hpp/cpp           # Per-function state, RAII-scoped around each function body
    │   ├── LLVMTypeHelpers.hpp             # Pure LLVM type and value utilities
    │   ├── Manifest.hpp                    # Plain-data contract between CodeGen and consumers
    │   ├── Program.hpp/cpp                 # Per-program state that outlives any single function body
    │   ├── Types.hpp/cpp                   # Lucid → LLVM type mapping
    │   │
    │   ├── emit/
    │   │   ├── Emitter.hpp/cpp             # The codegen emitter — one class, four public entry points
    │   │   ├── EmitClosure.cpp             # Closure lowering: environment construction, capture binding, body lowering, env-drop glue
    │   │   ├── EmitConcurrency.cpp         # Concurrency lowering: async/await/spawn/join emission
    │   │   ├── EmitDecl.cpp                # Declaration lowering — emit(DeclAST*) entry point and per-kind dispatch
    │   │   ├── EmitExpr.cpp                # Expression-lowering subsystem entry point: emit(ExprAST*) dispatch and const-fold short-circuit
    │   │   ├── EmitPlace.cpp               # Place construction and the single write path
    │   │   ├── EmitStmt.cpp                # Statement lowering — emit(StmtAST*) entry point and scope-management primitives
    │   │   │
    │   │   └── expr/
    │   │       ├── EmitAccess.cpp          # Reads from storage: index, slice, field access, module access, arena access
    │   │       ├── EmitAggregate.cpp       # Aggregate construction: struct literal, array literal
    │   │       ├── EmitCall.cpp            # Calls and coercion: emitCall, emitIntrinsic, coerceArgument
    │   │       ├── EmitScalar.cpp          # Scalar and control-flow expressions: literals, identifiers, binary, unary, if, range
    │   │       ├── EmitTruthiness.cpp      # Truthiness rules: emitTruthiness
    │   │       └── EmitWrite.cpp           # Read-modify-write expressions: assign, null-coalesce, pipeline
    │   │
    │   ├── intrinsic/
    │   │   ├── IntrinsicEmitter.hpp/cpp    # Intrinsic emission API base
    │   │   ├── LLVMIntrinsicEmitter.hpp/cpp# Low-level LLVM intrinsic emission
    │   │   └── LucidIntrinsicEmitter.hpp/cpp# Lucid-specific intrinsic emission logic
    │   │
    │   ├── ownership/
    │   │   ├── DropGlue.hpp/cpp            # Drop glue generation for owned types
    │   │   └── Ownership.hpp/cpp           # Memory management & ownership rules lowering
    │   │
    │   └── passes/                         # Codegen pass runner
    │       ├── Passes.hpp                  # Pass declarations and shared pass interface
    │       ├── DeclarePass.cpp             # Pass 1: emit all function prototypes and type declarations
    │       ├── DefinePass.cpp              # Pass 2: emit all function bodies
    │       └── ModulePass.cpp              # Pass 3: module-level finalization and manifest population
    │
    ├── interpreter/                        # ORC JIT backend (lucid run)
    │   ├── Interpreter.hpp/cpp             # Public API & orchestration logic
    │   │
    │   ├── core/
    │   │   ├── InterpreterContext.hpp      # Context holding interpreter state
    │   │   ├── InterpreterProgram.hpp/cpp  # Program-level state (modules, IDs, load/run)
    │   │   ├── InterpreterSession.hpp/cpp  # Session state (JIT, linker, instance buffer)
    │   │   └── ModuleRegistry.hpp/cpp      # Module dependency & load tracking
    │   │
    │   ├── dynlink/                        # Platform library dynamic linker
    │   │   ├── DynamicLinker.hpp/cpp       # Platform-agnostic library loader
    │   │   └── LibraryHandle.hpp/cpp       # RAII wrapper for dlopen/LoadLibrary
    │   │
    │   ├── jit/
    │   │   └── JITSession.hpp/cpp          # ORC JIT session management
    │   │
    │   └── support/
    │       ├── ExecutionResult.hpp         # Result structure of execution
    │       ├── InterpreterError.hpp        # Interpreter error types
    │       └── InterpreterOptions.hpp      # Execution options configuration
    │
    ├── compiler/                           # AOT backend (future implementation)
    │
    ├── stdlib/                             # Standard library (written in Lucid)
    │   └── simd.luc
    │
    ├── cli/                                # Command-line interface
    │   ├── CLIContext.hpp                  # Shared CLI context for a single run session
    │   ├── CLIOptions.hpp                  # Unified CLI options for all commands
    │   ├── DependencyGraph.hpp             # Bi-directional dependency graph for hot-reload
    │   ├── FileWatcher.hpp                 # File watcher for hot-reload
    │   ├── RunOptions.hpp                  # Options for the 'run' command
    │   │
    │   ├── commands/
    │   │   ├── emit-ir.hpp/cpp             # 'emit-ir' command - emit LLVM IR
    │   │   ├── parse.hpp/cpp               # 'parse' command - parse-only mode
    │   │   ├── run.hpp/cpp                 # 'run' command - JIT run (.luc source or .bc)
    │   │   └── sema.hpp/cpp                # 'sema' command - parse + semantic analysis
    │   │
    │   └── pipeline/
    │       ├── JSONDumper.hpp/cpp          # Serialization for AST nodes and diagnostics
    │       └── Pipeline.hpp/cpp            # Compiler pipeline with configurable stop points
    │
    └── debug/                              # Developer tools (not user-facing)
        ├── DebugMacros.hpp                 # Debug assertion macros
        └── DebugUtils.hpp                  # Debug printing & inspection utilities

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