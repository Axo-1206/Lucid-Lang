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
  - [4. Codegen](#4-codegen)
    - [What is emitted](#what-is-emitted)
    - [The Manifest](#the-manifest)
    - [The mapping](#the-mapping)
    - [Key files](#key-files)
  - [5. Backend — JIT Runner (ORC JIT)](#5-backend--jit-runner-orc-jit)
    - [Runner API](#runner-api)
    - [Load Sequence](#load-sequence)
    - [Reload Sequence](#reload-sequence)
    - [Foreign Symbol Resolution](#foreign-symbol-resolution)
    - [Quarantine](#quarantine)
    - [Key files](#key-files-1)
  - [6. Backend — AOT Compiler (future)](#6-backend--aot-compiler-future)
  - [7. Runtime](#7-runtime)
    - [The ABI Table (`functions.def`)](#the-abi-table-functionsdef)
    - [Runtime Implementation (`src/runtime/`)](#runtime-implementation-srcruntime)
  - [8. Standard Library](#8-standard-library)
  - [9. Distribution — File Extensions, Libraries, and Bitcode](#9-distribution--file-extensions-libraries-and-bitcode)
    - [9.1 File Extensions](#91-file-extensions)
    - [9.2 `.luci` — the Library Interface File](#92-luci--the-library-interface-file)
    - [9.3 Static vs. Shared Libraries](#93-static-vs-shared-libraries)
    - [9.4 `.bc` — Portable Bitcode Distribution](#94-bc--portable-bitcode-distribution)
    - [9.5 Whole-Program Module Model](#95-whole-program-module-model)
    - [9.6 Calling Another Lucid Library at Runtime](#96-calling-another-lucid-library-at-runtime)
  - [10. CLI and LSP](#10-cli-and-lsp)
    - [CLI (`src/cli/`)](#cli-srccli)
    - [LSP (`src/lsp/`)](#lsp-srclsp)
  - [11. File Structure](#11-file-structure)
  - [Appendix — Pipeline at a glance](#appendix--pipeline-at-a-glance)

---

## 1. Overview

The Lucid Compiler is a single binary (`lucid`) that serves two
modes from a shared frontend:

- **`lucid run`** — runs a `.luc` source file (or a pre-compiled `.bc`
  file) immediately using LLVM's ORC JIT. No output file is produced.
- **`lucid build`** — compiles `.luc` files ahead-of-time (AOT) using LLVM
  and the system linker, producing a native binary, a shared/static
  library, or portable bitcode.

Both modes share the same frontend (lexer, parser, semantic analysis) and
the same codegen pass. The split is what happens to the resulting
`CodegenResult` — the JIT path compiles it in memory and executes it; the
AOT path writes an object file and invokes the system linker.

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
                    │    Codegen      │  CodegenResult
                    │                 │  (llvm::Module + Manifest)
                    └────────┬────────┘
                             │
              ┌──────────────┴──────────────┐
              │                             │
   ┌──────────▼──────────┐     ┌────────────▼─────────────┐
   │   JIT Runner        │     │    AOT Compiler          │
   │  (lucid run)        │     │  (lucid build)           │
   │                     │     │                          │
   │  add IR to JIT      │     │  optimize IR             │
   │  dlopen @[link]     │     │  emit object file        │
   │  execute            │     │  invoke system linker    │
   └─────────────────────┘     └──────────────────────────┘
```

> **NOTE — Pipeline layering.** The CLI owns the frontend and codegen.
> The CLI calls `parse`, `sema`, and `codegen::generate`, producing a
> `CodegenResult`. The backends (`jit-runner/` and, in the future,
> `compiler/aot/`) consume the `CodegenResult`. Neither backend sees
> source files, ASTs, or file versions. This is the load-bearing
> architectural boundary; the rest of this document assumes it.

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
> - **JIT (ORC) and AOT share the same codegen** and most of the same
>   LLVM codegen, so supporting both `run` and `build` costs far less than
>   the first backend does — it's not roughly double.
> - Standard `-ffunction-sections -fdata-sections` + `--gc-sections`/`/OPT:REF`
>   and symbol stripping apply to this binary exactly as they do to the
>   kernel — LLVM is one more static library being linked, not a special case.
> - If `luc_langserver` ends up as a separate binary from `lucid`, it only
>   needs the frontend (lexer/parser/sema) for diagnostics and autocomplete —
>   it never touches codegen, so it doesn't need to link LLVM at all.
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
before codegen begins. No IR is generated until the AST is clean.

**Step 5 — Codegen**
`codegen::generate(modules, pool, diagnostics)` translates the validated
AST into a single `llvm::Module` for the whole program, plus a
`Manifest` describing the program's host-facing interface. It returns a
`CodegenResult` containing both.

The mapping is direct and deterministic — every Lucid construct has
exactly one IR pattern (see §4 for the table). Every module-level
binding lives in a per-module instance `GlobalVariable`; every function
from every module lives in the same `llvm::Module`; every type lives in
the same `LLVMContext`. There is one module, one context, one symbol
table.

The CLI orchestrates: parse → sema → `codegen::generate` → backend.

**Step 6a — JIT Runner (run mode)**
The CLI passes the `CodegenResult` to a `jit_runner::JITRunner`. The
runner adds the module to an LLVM ORC JIT session, loads the foreign
libraries named in the manifest, invokes the program initializer, and
calls the entry point. See §5.

**Step 6b — AOT Compiler (build mode)**
The AOT backend receives the same `CodegenResult`. It runs the LLVM
optimisation pipeline on the single whole-program module and emits a
native object file. The `Linker` collects the foreign library names from
the manifest and constructs a linker invocation (`ld`, `lld`, or
`link.exe` depending on platform). See §6.

**Step 6c — Bitcode Emission (`lucid build --emit-bc`)**
The CLI runs target-independent optimisation passes on the module, then
serializes it with `llvm::BitcodeWriter`. See §9.4.

---

## 3. Frontend

The frontend is the entire pipeline from source text to validated AST. It is
shared between both modes and never touches LLVM. All three stages (lexer,
parser, sema) must complete successfully before any codegen is produced.

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

## 4. Codegen

**Location:** `src/codegen/`

Codegen translates the validated AST to LLVM IR. It is the **only**
place in the codebase that imports LLVM headers. Both the JIT runner and
the AOT backend consume the output of this stage.

The public API is one function:

```cpp
namespace codegen {
    CodegenResult generate(
        const std::vector<ModuleAST*>& modules,   // dependency order
        StringPool& pool,
        DiagnosticEngine& diagnostics);
}
```

`CodegenResult` contains a single `llvm::Module` for the whole program and
a `Manifest` describing the program's host-facing interface. The LLVM
context is owned by the result.

### What is emitted

**One `llvm::Module` per program.** It contains:

- One `llvm::StructType` per source module — `%module_<sanitized>`, with one
  field per top-level `let`/`const`/`cls`-shaped `FuncDeclAST`.
- One `llvm::GlobalVariable` per module — `@__module_state_<sanitized>`,
  `InternalLinkage`, zero-initialized. This is the module's storage.
- Every function from every module.
- Every type from every module.
- `__lucid_program_init` — initializes every module's instance fields.
- `__lucid_program_free` — releases every module's resources in reverse
  declaration order.

Every type, function, and global lives in the same `LLVMContext` and the
same symbol table. There is no cross-module swap, no instance table, no
per-module init/free pair.

**Module-level access** is a direct GEP into the module's state global:

```llvm
%field = getelementptr %module_M, ptr @__module_state_M, i64 0, i64 i
%v = load i32, ptr %field
```

Two instructions per access. No null check needed (the global exists from
the moment the module loads).

### The Manifest

`Manifest` is the plain-data contract between codegen and its consumers.
It contains:

- `entry.symbol` — the linker-level symbol name of the program's entry point.
- `programInitSymbol` / `programFreeSymbol` — always emitted.
- `modules` — one entry per source module, with its state symbol name.
- `runtimeSymbols` — the runtime ABI functions the module references.
- `foreignLibraries` — the `@[link("...")]` library names.

The JIT runner and the AOT backend both consume the manifest rather than
re-deriving its contents from the module or the AST.

### The mapping

| Lucid construct                 | LLVM IR output                  |
| ------------------------------- | ------------------------------- |
| `const f (x T) -> R`            | `define R @f(T %x)`             |
| `let x T = v` (function-local)  | `alloca T` + `store`            |
| `let x T = v` (module-level)    | field in `@__module_state_<M>`  |
| `@[foreign("C")] const g (...)` | `declare` (unresolved symbol)   |
| `#sqrt(x)`                      | `call @llvm.sqrt.f32(float %x)` |
| `#memcpy(d, s, n)`              | `call @llvm.memcpy(...)`        |
| `#toRef(p)`                     | non-null assertion + `bitcast`  |
| `*T` (raw pointer)              | `ptr` (LLVM opaque pointer)     |
| `T?` / `T!` / `T?!`             | `{ i8 tag, T value }` struct    |
| `string`                        | `{ ptr, i64 len, i64 cap }`     |

### Key files

- `CodeGen.hpp/cpp` — the public entry point (`generate`) and `CodegenResult`.
- `Program.hpp/cpp` — per-program state: LLVM context, module, builder, type
  cache, ABI surface, ownership engine, emitter.
- `Types.hpp/cpp` — Lucid type → LLVM type mapping, with a per-program cache.
- `Abi.hpp/cpp` — the runtime ABI surface, generated from `runtime-abi/functions.def`.
- `FunctionState.hpp/cpp` — per-function state (scope stack, loop stack,
  value bindings), RAII-scoped around each function body.
- `FailureKind.hpp/cpp` — the closed set of runtime failures the compiler
  emits, plus the `isRiskyLhs` predicate that drives `??` on risky operations.
- `Manifest.hpp` — the plain-data contract.
- `LLVMTypeHelpers.hpp` — pure type/value utilities.
- `emit/` — the emitter (one class, four public entry points): `Emitter.hpp/cpp`,
  `EmitDecl.cpp`, `EmitStmt.cpp`, `EmitPlace.cpp`, `EmitClosure.cpp`,
  `EmitConcurrency.cpp`, and the `emit/expr/` subdirectory for expression
  emitters (`EmitExpr.cpp`, `EmitScalar.cpp`, `EmitTruthiness.cpp`,
  `EmitAccess.cpp`, `EmitAggregate.cpp`, `EmitWrite.cpp`, `EmitCall.cpp`).
- `intrinsic/` — intrinsic dispatch and emission (`IntrinsicEmitter.hpp/cpp`,
  `LLVMIntrinsicEmitter.hpp/cpp`, `LucidIntrinsicEmitter.hpp/cpp`).
- `ownership/` — the ownership engine (`Ownership.hpp/cpp`) and lazy
  drop/copy glue (`DropGlue.hpp/cpp`).
- `passes/` — the three-pass runner: `DeclarePass.cpp` (types and
  prototypes), `DefinePass.cpp` (function bodies), `ModulePass.cpp`
  (module state globals, `__lucid_program_init` / `__lucid_program_free`,
  manifest population).

---

## 5. Backend — JIT Runner (ORC JIT)

**Location:** `src/jit-runner/`

The JIT runner backend receives a `CodegenResult` from the CLI and
executes it immediately using LLVM's ORC (On-Request Compilation) JIT
framework. No object file is written to disk.

> **NOTE — the name.** "Interpreter" was the historical name for this
> folder. It is a misnomer: there is no interpretation. The module is
> compiled to native machine code in memory and invoked directly. The
> folder is `jit-runner/`, and its public class is `JITRunner`.

### Runner API

The runner's entire public interface is five methods:

```cpp
namespace jit_runner {

class JITRunner {
public:
    JITRunner(StringPool& pool,
              DiagnosticEngine& diag,
              const JITRunnerOptions& options);
    ~JITRunner();

    /// Initialize the JIT session. Idempotent.
    void initialize();

    /// Load a program from a codegen result. Replaces any existing
    /// program, freeing it first. Returns false on failure (diagnostic
    /// already emitted).
    bool load(codegen::CodegenResult result);

    /// Replace the current program with a new one. Frees the old
    /// program's resources, installs the new module, quarantines the
    /// old module. Delegates to load() if no program is loaded.
    bool reload(codegen::CodegenResult result);

    /// Run the entry point. Returns the exit code and timing.
    ExecutionResult run();

    /// Tear down the current program (if any). Idempotent.
    void close();
};

}
```

**What the runner does NOT do:**

- It does not parse, sema, or codegen. Its input is a `CodegenResult`.
- It does not track source files or their versions. That's the CLI.
- It does not hold a `ModuleAST*`. Its input carries everything it needs
  in the manifest.
- It does not decide when to reload. The CLI decides, and calls `reload`.

### Load Sequence

The runner's `load` takes a `CodegenResult` and:

1. **Loads foreign libraries.** For each name in
   `manifest.foreignLibraries`, call `JITSession::loadLibrary(name)`.
   Loading is idempotent; already-loaded libraries are skipped.

2. **Adds the program module to the JIT.** The runner calls
   `JITSession::addModule(std::move(result.module), "__lucid_program__")`,
   which returns a `ResourceTrackerSP` identifying the module within the
   JIT's symbol table.

3. **Runs the program initializer.** The runner looks up
   `manifest.programInitSymbol` (`__lucid_program_init`) via
   `JITSession::lookupSymbol` and calls it. This writes each module's
   initial values into its state global.

4. **Reads and stores the entry symbol.** From `manifest.entry.symbol`.
   If empty, `load` succeeds but `run` will fail with a clear diagnostic.

### Reload Sequence

The runner's `reload` takes a new `CodegenResult` and:

1. **Frees the old program's resources.** Calls
   `manifest.programFreeSymbol` (`__lucid_free_program`), which runs every
   module's free function in reverse dependency order.

2. **Loads any new foreign libraries** named in the new manifest.

3. **Adds the new program module to the JIT**, capturing a fresh
   `ResourceTrackerSP`.

4. **Runs the new program initializer.** `__lucid_program_init` from the
   new module writes fresh values into the new state globals.

5. **Quarantines the old module.** The old `ResourceTrackerSP` is passed
   to `JITSession::quarantine`, which removes its symbols from the JIT's
   symbol table.

The order matters:

- Free *before* generating and installing the new module. The old
  `__lucid_free_program` symbol is only resolvable while the old module is
  still in the JIT.
- Init new *after* adding the new module. `__lucid_program_init` is a
  symbol in the new module.
- Quarantine old *after* the new module is installed and initialized. The
  old module's code might still be referenced by an in-flight stack frame;
  removing it before the new module is usable leaves a window where the
  code is unreachable.

**State does not survive a reload.** The old program's resources are
released; the new program's globals are freshly initialized from the
source. Preserving state across a reload is a codegen feature (a
migration function emitted alongside the fresh init), not a runner
feature; it is Tier 2 work and not implemented.

### Foreign Symbol Resolution

For each library named in the manifest's `foreignLibraries`, the runner
calls `JITSession::loadLibrary(name)`. The implementation:

1. On Linux/macOS, calls `dlopen(name, RTLD_NOW | RTLD_GLOBAL)`.
2. On Windows, calls `LoadLibraryA(name)`.
3. The loaded library's symbols become available to the JIT via LLVM's
   `DynamicLibrarySearchGenerator::GetForCurrentProcess`, which the JIT
   session installs at initialization.

When JIT-compiled code calls a foreign function, the symbol is resolved
from this table — a direct function call, no marshaling layer.

`luc_kernel.dll` is loaded before any user module if the manifest names it.
Its symbols cover the `lge_*.h` API surface. Other libraries named in
`@[link]` are loaded on demand.

### Quarantine

When a program is reloaded, the old module's machine code is removed from
the JIT's symbol table but not immediately freed. The reason is that an
in-flight stack frame — a function that was executing when the reload was
triggered — might still be inside the old code.

`JITSession::quarantine(ResourceTrackerSP)` handles this. On a
single-threaded runner (the current design), `quarantine` removes the
tracker immediately: the caller has confirmed it is at a safe point
(between frames), and no thread can be executing in the old module.

If multi-threaded execution is added later, `quarantine` becomes a queue,
and a `sweepQuarantine()` method runs at safe points to release the
trackers whose grace periods have elapsed. The API is shaped to support
this without changing its callers.

### Key files

- `JITRunner.hpp/cpp` — the public facade.
- `JITProgram.hpp/cpp` — the internal per-program state: the current
  `CodegenResult`, the current `ResourceTrackerSP`, and the load / reload
  / teardown sequences.
- `JITRunnerOptions.hpp` — run options (optimization level, verbose, etc.).
- `jit/JITSession.hpp/cpp` — the ORC `LLJIT` wrapper: `addModule`,
  `lookupSymbol`, `quarantine`, `loadLibrary`.
- `dynlink/DynamicLinker.hpp/cpp` — platform-agnostic library loader,
  used by `JITSession`.
- `dynlink/LibraryHandle.hpp/cpp` — RAII wrapper for `dlopen` / `LoadLibrary`.
- `support/ExecutionResult.hpp` — the runner's output: exit code, success
  flag, timing, entry symbol.
- `support/JITRunnerError.hpp` — the runner's exception type.

---

## 6. Backend — AOT Compiler (future)

**Location:** `src/compiler/aot/` (not yet implemented)

The AOT backend receives the same `CodegenResult` the JIT runner does and
produces a native binary, a shared library, or portable bitcode.

`AOT` sets up an `llvm::TargetMachine` for the target platform — selecting
`Reloc::PIC_` for shared libraries, the platform default otherwise, since
position-independent codegen is a `TargetMachine` concern, not something
codegen needs to know about — runs `llvm::PassManager` with the requested
optimisation level (`-O0` through `-O3`), and writes the object file via
`llvm::raw_fd_ostream`.

`Linker` collects the foreign library names from the manifest and
assembles a linker invocation. On Linux: `ld` or `lld` with `-lname` flags.
On Windows: `link.exe` with `/lib:name.lib`. The result is a native `.exe`,
`.dll`, `.a`, or `.so` depending on the target.

Because codegen produces one `llvm::Module` for the whole program, there
is no cross-module merge step. The AOT pipeline is: optimise the single
module, emit an object file, invoke the linker.

**Key files (planned):**
- `aot/AOT.hpp/cpp` — `TargetMachine` setup, optimisation pipeline, object
  file emission.
- `aot/Linker.hpp/cpp` — assembles and invokes the system linker from the
  manifest's `foreignLibraries`.

---

## 7. Runtime

**Location:** `src/runtime/` and `src/runtime-abi/`

The runtime provides support services that execute alongside the compiled
or JIT-executed Lucid program. Unlike the frontend and backends, the
runtime is linked into the final binary and runs at program execution time.

### The ABI Table (`functions.def`)

**Location:** `src/runtime-abi/functions.def`

`functions.def` is the single source of truth for the runtime ABI surface.
It is an X-macro table with one row per runtime function. Each row names
the function's enumerator, its linker-level symbol name, its return type
tag, and its parameter type tags.

The table has four consumers, all reading the same rows:

1. `codegen/Abi.hpp` — emits the `RuntimeFn` enumerators and one variadic
   member template per row (`abi.Alloc(builder, size)` etc.).
2. `codegen/Abi.cpp` — builds the `RuntimeFn → (symbol, tags)` table from
   which the `llvm::FunctionType` and its attributes are built.
3. `runtime-abi/lucid_runtime.h` — declares the `extern "C"` prototype of
   every row. Every file in `src/runtime/` includes it, so a definition
   that disagrees with its row is a "conflicting declaration of C function"
   error in that file.
4. `runtime/exports.cpp` — takes the address of every row's function, so
   a row with no definition is a link error naming the symbol.

Because all four read the same table, their views cannot drift.

The ABI's struct layouts (`LucidString`, `LucidSlice`, `LucidArena`,
`LucidClosure`, `LucidClosureHeader`) are pinned in
`runtime-abi/lucid_abi.h`, with `static_assert`s on sizes, offsets,
alignment, and triviality. Every consumer derives its own view from these
pins.

**Key files:**
- `runtime-abi/functions.def` — the X-macro table.
- `runtime-abi/lucid_abi.h` — the C-layout structs and named scalar types.
- `runtime-abi/lucid_runtime.h` — the `extern "C"` prototypes.

### Runtime Implementation (`src/runtime/`)

The runtime implements the functions declared by the table. It is written
in C++ and links against the LLVM-generated code via the ABI contracts in
`lucid_abi.h`.

- `StringRuntime.cpp` — string operations: `str_concat`, `str_slice`,
  `str_eq`, `str_from_ptr`, and the scalar formatters (`int_to_str`,
  `float_to_str`, `bool_to_str`, `char_to_str`, `uint_to_str`,
  `ptr_to_hex_string`).
- `MemoryRuntime.cpp` — `__lucid_alloc`, `__lucid_free`, the allocation
  registry, and `__lucid_leak_report`.
- `ArenaRuntime.cpp` — `__lucid_arena_create`, `__lucid_arena_alloc`,
  `__lucid_arena_reset`, `__lucid_arena_free`, and the query functions.
- `ClosureRuntime.cpp` — `__lucid_alloc_env`, `__lucid_retain_env`,
  `__lucid_release_env`.
- `ConcurrencyEntry.cpp`, `ConcurrencyRuntime.hpp/cpp` — `__lucid_async`,
  `__lucid_await`, `__lucid_spawn`, `__lucid_join`, `__lucid_shutdown`,
  the thread pool, and the event loop.
- `PanicRuntime.cpp` — `__lucid_panic`.
- `ClosureEnvironment.hpp` — the internal closure environment layout,
  pinned against `LucidClosureHeader` in `lucid_abi.h`.
- `RuntimeInternal.hpp` — internal runtime helpers shared across the
  runtime translation units. Not part of the ABI; not prefixed with
  `__lucid_`.
- `exports.cpp` — takes the address of every runtime function, forcing the
  linker to check that each is defined.

Every `__lucid_*` symbol is a row in `functions.def`. Internal helpers
live in namespace `lucid::runtime`, not under the `__lucid_` prefix.

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

## 9. Distribution — File Extensions, Libraries, and Bitcode

This section covers what happens once a Lucid file needs to leave the
project it was written in — as a library another project links against, or
as a portable compiled artifact handed to someone without the `.luc`
source. None of this changes the frontend or codegen; it's entirely about
what happens to the `CodegenResult` they produce, downstream.

### 9.1 File Extensions

| Extension                         | What it is                                                                                             | Produced by                                         | Consumed by                                                                                        |
| --------------------------------- | ------------------------------------------------------------------------------------------------------ | --------------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| `.luc`                            | Lucid source                                                                                           | the developer                                       | frontend (lexer → parser → sema)                                                                   |
| `.o`                              | Object file — one translation unit's machine code, unlinked                                            | AOT backend, internal intermediate                  | system linker only; not a first-class CLI output                                                   |
| `.a`                              | Static library archive                                                                                 | `lucid build --lib` (default / `--static`)          | system linker only, at build time. Cannot be `dlopen`'d                                            |
| `.so` / `.dylib` / `.dll`         | Shared/dynamic library                                                                                 | `lucid build --lib --shared`                        | system linker (if linked at build time) **or** dynamic linker (`dlopen`/`LoadLibrary`) at run time |
| `.lib` (Windows only)             | Import stub for a `.dll`                                                                               | alongside `.dll`, on Windows shared builds          | system linker (`link.exe`) only                                                                    |
| `.luci`                           | Lucid interface file — exported signatures, types, struct layouts, doc comments; no bodies             | `lucid build --lib` (any static/shared combination) | Sema's `import` resolution, and the LSP (one format, two consumers — see §9.2)                     |
| `.bc`                             | LLVM bitcode — a serialized `llvm::Module`, target-agnostic, frozen before any target-specific codegen | `lucid build --emit-bc` (see §9.4)                  | `lucid run <file>.bc` — same JIT runner, just skipping lexer/parser/sema/codegen                   |
| `.lfi` (existing — `lge_ffi.lfi`) | C-side symbol table for `@[foreign("C")]` validation                                                   | shipped with the Engine SDK                         | `FFIValidator` only, at Sema time                                                                  |

### 9.2 `.luci` — the Library Interface File

Compiled machine code carries no types — a `.o`/`.a`/`.so` gives you a
symbol table at best, never parameter types, struct layouts, or which
symbols are meant to be called externally versus internal names that
happen to leak. `.luci` is the Lucid-native equivalent of a C header,
generated by the compiler instead of hand-written: a serialized projection
of the typed AST's exported subset (everything the module system already
tracks via `@[export]`), including doc comments, so hover/autocomplete
work without source.

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
- **Default is `--static`.** Case A in §9.6 (importing a known library at
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
  `.luci`.

| Flag                                          | Produces                                                                                             |
| --------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| `lucid build --lib lib.luc`                   | `lib.a` + `lib.luci` (default)                                                                       |
| `lucid build --lib --static lib.luc`          | `lib.a` + `lib.luci` (explicit spelling of the default)                                              |
| `lucid build --lib --shared lib.luc`          | `lib.so`/`.dylib`/`.dll` (+ `.lib` stub on Windows) + `lib.luci`                                     |
| `lucid build --lib --static --shared lib.luc` | all of the above, one shared `.luci` — only when genuinely needed, accepting the double-codegen cost |

### 9.4 `.bc` — Portable Bitcode Distribution

`.bc` is not a new execution engine — it's `llvm::BitcodeWriter` serializing
the exact same `llvm::Module` type codegen already produces, frozen at
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
                                     skips lexer/parser/sema/codegen
                                     entirely, hands it to the same
                                     JIT runner used for source
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

> **NOTE — the manifest in a `.bc` file.** The `CodegenResult` that the
> JIT runner consumes has two parts: the `llvm::Module` and the
> `Manifest`. The `.bc` file serializes the module but not the manifest.
> Some manifest fields are derivable from the module (the entry symbol, the
> program init/free symbols, the runtime symbols the module references).
> **Foreign library names are not.** A `.bc` produced from source that
> used `@[link("opengl")]` does not carry that name in any form the
> module reader can recover.
>
> This is an open design decision. Options under consideration:
> 1. Serialize foreign library names into the module's named metadata
>    (`!lucid.foreign_libraries = !{!"opengl"}`) and read them back on load.
> 2. Require `lucid run <file>.bc --link <name>` flags to supply them
>    alongside the file.
> 3. Write a small companion file (`<name>.bc.meta`) next to the `.bc`.
>
> None of these exist today. Any implementation of `.bc` distribution has
> to answer this question first.

### 9.5 Whole-Program Module Model

Codegen produces one `llvm::Module` for the entire program, not one per
source file. This is the model both the JIT runner and the AOT compiler
consume.

**Implications:**

- **Cross-file references are resolved at codegen time.** The type system,
  symbol table, and module state globals all live in one module. There is
  no `declare` stub for a cross-file call and no `llvm::Linker::linkModules`
  merge step.
- **Hot-reload granularity is the whole program.** Any source change
  triggers a full re-codegen. Reload swaps the whole program module, not
  a per-file unit.
- **Registering two modules with the same symbol is impossible** in the
  same `JITDylib`. This is why reload is destructive: the old module must
  leave before the new one arrives (see §5, Reload Sequence).

**Why this model:**

- Simpler codegen: one context, one module, one symbol table, no
  cross-module bookkeeping.
- Simpler AOT: no merge step, no per-file boundaries to reason about
  during optimisation.
- Simpler runner: one module per program, one tracker, one init/free pair.

**What it costs:**

- Hot-reload granularity is the whole program, not per-file. A change to
  one source file triggers re-codegen of everything. For the target use
  case (fast iteration on a game), this is a few hundred milliseconds and
  acceptable.

Per-file hot-reload (swap only the changed file's IR) would require
splitting codegen back into per-file modules and re-introducing cross-module
symbol resolution. That was the previous model and was deliberately
removed; the current one-module model is the design.

### 9.6 Calling Another Lucid Library at Runtime

Two distinct cases, handled by different mechanisms:

**Case A — `import lib;`, known at compile time.** `Codegen` emits a
`declare` for each imported symbol exactly as it does for
`@[foreign("C")]`. At JIT time, the runner's `JITSession::loadLibrary`
`dlopen`s the library and registers its symbols with the JIT's
`DynamicLibrarySearchGenerator`, same as any `@[link(...)]` target; at
AOT time, `Linker` (§6) passes it as a normal `-l` flag. The only new work
is `LibValidator` (§9.2) checking the call site against `.luci` instead of
`lge_ffi.lfi` — everything downstream of Sema is unchanged.

**Case B — genuinely dynamic (plugin-style) loading, where the library's
identity isn't known until runtime.** No `.luci` can be checked against,
because which file to load is itself a runtime value — this needs an
explicit, visible, unsafe-flavored API, the same idiom C's `dlsym` uses:
`dynlib:load(path) -> DynLibrary!` / `dynlib:symbol(handle, name) -> *void?`,
with the caller responsible for `#bitcast`ing the result to whatever
function-pointer type they believe is correct. Implementation-wise, this
is a thin `extern "C"` pair (`__lucid_dynlib_load` /
`__lucid_dynlib_symbol`) wrapping the existing `dynlink/DynamicLinker` +
`LibraryHandle` machinery, exposed as a `stdlib/dynlib.luc` module.

**This needs to work under AOT too, which is a gap today.**
`compiler/aot/Linker.hpp/cpp` (when implemented) will only do compile-time
linking. For `dynlib:load(...)` to work inside a `lucid build`-produced
binary (not just under `lucid run`, where the JIT's `DynamicLinker`
already exists), the `__lucid_dynlib_*` entry points need to be part of
the Lucid runtime statically linked into every AOT binary. Worth testing
under AOT specifically once built.

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
lucid run   <file.luc>                      -- parse + sema + codegen, then
                                                JIT-execute; no file output
lucid run   <file.bc>                       -- read bitcode, JIT-execute;
                                                skips lexer/parser/sema/codegen
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

**`lucid run` dispatches on file extension.** `.luc` is parsed, sema'd,
and codegen'd; `.bc` is read via `llvm::BitcodeReader` and wrapped in a
`CodegenResult` with a reconstructed manifest. Both paths produce a
`CodegenResult` that is handed to the same `JITRunner`. There is one
`run` command, not two, matching the precedent set by LLVM's `lli`.

Flags relevant to `run` (`--verbose`, `--trace`, `--no-hot-reload`,
`-O<level>`, `--entry <name>`) apply unchanged to both the `.luc` and
`.bc` forms of `run`.

**Hot reload.** With `--no-hot-reload` off (the default), `lucid run
<file.luc>` starts a `FileWatcher` (see `cli/FileWatcher.hpp`) in a
background thread. When a watched file changes, the watcher's callback
re-parses, re-semas, and re-codegens the affected modules, then hands the
new `CodegenResult` to `JITRunner::reload`. The runner frees the old
program, installs the new module, and quarantines the old one (see §5).

The CLI owns all of this: file watching, module re-resolution, the
re-parse/sema/codegen cycle, and the decision of when to reload. The
`JITRunner` never sees a file path or an AST; it takes a `CodegenResult`
and makes it runnable.

`lucid run <file.bc>` does not support hot reload; the `.bc` file is a
frozen artifact, and there is no source to recompile.

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

The server implements the Language Server Protocol so editors
(VS Code, Neovim, etc.) can provide:
- Real-time diagnostics (errors and warnings as you type)
- Autocomplete for identifiers, fields, and type annotations
- Go-to-definition and find-references
- Hover documentation from doc-comments

The LSP server reuses the lexer, parser, and semantic analysis passes on each
file change, running them incrementally where possible. It runs as a separate
process and communicates with the editor via stdin/stdout JSON-RPC. It never
touches codegen or LLVM, and the `luc_langserver` binary is expected to link
no LLVM at all.

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
    │   │   ├── DiagCode.hpp                # Diagnostic error codes
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
    │   ├── lucid_abi.h                     # ABI contract between CodeGen, runtime, and JIT runner
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
    │       └── ModulePass.cpp              # Pass 3: module state globals, program init/free, manifest population
    │
    ├── jit-runner/                         # ORC JIT backend (lucid run)
    │   ├── JITRunner.hpp/cpp               # Public facade: initialize / load / reload / run / close
    │   ├── JITProgram.hpp/cpp              # Internal: one CodegenResult, one ResourceTrackerSP
    │   ├── JITRunnerOptions.hpp            # Run options
    │   │
    │   ├── jit/
    │   │   └── JITSession.hpp/cpp          # ORC LLJIT wrapper: addModule, lookupSymbol, quarantine, loadLibrary
    │   │
    │   ├── dynlink/                        # Platform library dynamic linker
    │   │   ├── DynamicLinker.hpp/cpp       # Platform-agnostic library loader
    │   │   └── LibraryHandle.hpp/cpp       # RAII wrapper for dlopen/LoadLibrary
    │   │
    │   └── support/
    │       ├── ExecutionResult.hpp         # Result structure of execution
    │       └── JITRunnerError.hpp          # Exception type for JIT runner errors
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
├── jit-runner/
│   ├── test_load.cpp
│   ├── test_reload.cpp
│   └── test_teardown.cpp
├── compiler/
│   ├── test_codegen.cpp
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

---

## Appendix — Pipeline at a glance

For the reader who wants the whole flow in one place:

```
Source file (.luc)
    │
    ▼
[CLI] parse ──────────► AST
    │
    ▼
[CLI] sema ───────────► validated AST
    │
    ▼
[CLI] codegen::generate ───► CodegenResult
                             (one llvm::Module + Manifest)
    │
    ├───────────────┬────────────────┬───────────────────┐
    ▼               ▼                ▼                   ▼
[CLI] AOT      [CLI] Bitcode    [CLI] JITRunner     [Tooling]
emit .o        Writer .bc       ::load(result)      (e.g. emit-ir)
link → exe     │                ::run()
               │                    │
               │                    ▼
               │             [ORC LLJIT] compile → execute
               │
               ▼
        [later] read .bc → wrap in CodegenResult → JITRunner::load
```

**Ownership at each stage:**

| Stage           | Owner  | Input                    | Output            |
| --------------- | ------ | ------------------------ | ----------------- |
| Lexing          | CLI    | source text              | tokens            |
| Parsing         | CLI    | tokens                   | AST               |
| Sema            | CLI    | AST                      | validated AST     |
| Codegen         | CLI    | validated AST            | `CodegenResult`   |
| AOT             | CLI    | `CodegenResult`          | `.o` → native bin |
| Bitcode write   | CLI    | `CodegenResult`          | `.bc`             |
| JIT load/reload | Runner | `CodegenResult`          | (internal state)  |
| JIT run         | Runner | entry symbol in manifest | `ExecutionResult` |
| File watching   | CLI    | filesystem               | change callbacks  |

The runner never appears in the left column. That is the point of the
design.
