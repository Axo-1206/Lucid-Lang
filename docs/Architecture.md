# Lucid Compiler — Architecture

> This document describes the internal architecture of the Lucid language runtime: how source flows from text to bytecode, how the major subsystems are structured, and how each folder in the codebase fits into that flow.
>
> **Lucid is a bytecode-interpreted embedded scripting language.** There is no LLVM dependency in the base product, no JIT, and no ahead-of-time compiler. Source is compiled to bytecode and executed by an interpreter. Future backends (LucidJIT, LucidAOT) may be added later as optional tiers on top of the same bytecode; they are not part of this document.
>
> **Lucid is designed to be embedded.** The language runtime is a standalone library. The game engine that embeds it is a *consumer* of the runtime's host-registration API. The runtime knows nothing about game-specific types; the engine registers its own via `#host(...)`.
>
> **LLVM is not required.** No `llvm-config`, no LLVM version matching, no large static library. The runtime builds with a C++ compiler and nothing else.

---

## Table of Contents

- [Lucid Compiler — Architecture](#lucid-compiler--architecture)
  - [Table of Contents](#table-of-contents)
  - [1. Overview](#1-overview)
    - [What Lucid is not](#what-lucid-is-not)
    - [Future backends (not in this document)](#future-backends-not-in-this-document)
    - [The layers](#the-layers)
  - [2. Execution Pipeline](#2-execution-pipeline)
  - [3. Frontend](#3-frontend)
    - [3.1 Lexer](#31-lexer)
    - [3.2 Parser](#32-parser)
    - [3.3 Semantic Analysis](#33-semantic-analysis)
  - [4. Bytecode](#4-bytecode)
    - [4.1 What the compiler emits](#41-what-the-compiler-emits)
    - [4.2 The opcode set](#42-the-opcode-set)
    - [4.3 The bytecode module format](#43-the-bytecode-module-format)
    - [4.4 The `.lucb` archive](#44-the-lucb-archive)
    - [4.5 Key files](#45-key-files)
  - [5. Interpreter](#5-interpreter)
    - [5.1 The execution loop](#51-the-execution-loop)
    - [5.2 Values and the stack](#52-values-and-the-stack)
    - [5.3 Frames and calls](#53-frames-and-calls)
    - [5.4 The runtime library interface](#54-the-runtime-library-interface)
    - [5.5 Hot reload](#55-hot-reload)
    - [5.6 Key files](#56-key-files)
  - [6. Runtime Library](#6-runtime-library)
    - [6.1 The ABI table (`functions.def`)](#61-the-abi-table-functionsdef)
    - [6.2 Runtime implementation](#62-runtime-implementation)
  - [7. Host Registry](#7-host-registry)
    - [7.1 The registration model](#71-the-registration-model)
    - [7.2 Compile-time validation](#72-compile-time-validation)
    - [7.3 The embedding API](#73-the-embedding-api)
  - [8. Standard Library](#8-standard-library)
  - [9. Distribution](#9-distribution)
    - [9.1 File extensions](#91-file-extensions)
    - [9.2 The `.lucb` bytecode archive](#92-the-lucb-bytecode-archive)
    - [9.3 Whole-program module model](#93-whole-program-module-model)
  - [10. CLI and LSP](#10-cli-and-lsp)
    - [10.1 CLI](#101-cli)
    - [10.2 LSP](#102-lsp)
  - [11. File Structure](#11-file-structure)
  - [Appendix — Pipeline at a glance](#appendix--pipeline-at-a-glance)

---

## 1. Overview

Lucid is a bytecode-interpreted embedded scripting language. A `.luc` source file is parsed, type-checked, compiled to bytecode, and executed by an interpreter that runs inside the host process. There is no compilation to native code in the base product, no LLVM dependency, and no separate link step.

The runtime is a single library (`liblucid`) that an application embeds. The application is the *host*: it registers its own C++ functions and types with the runtime, and Lucid code calls them via `#host(...)` bindings. The runtime never knows anything about the host's types — the host registry is the entire interface.

The `lucid` CLI is a thin driver of the same library. `lucid run main.luc` compiles and executes a script; `lucid parse` and `lucid sema` stop the pipeline early for debugging and tooling.

### What Lucid is not

- **Not a compiled-to-native language.** The base product has no AOT backend. Compilation ends at bytecode.
- **Not LLVM-backed.** There is no LLVM dependency, no IR, no object files, no linker invocation.
- **Not a system language.** Lucid is designed to be embedded. It does not ship as a compiler that produces native executables; it ships as a runtime that an application links against.

### Future backends (not in this document)

Lucid may later gain optional backends that consume the same bytecode the interpreter consumes:

- **LucidJIT** — compiles bytecode to native code in memory for hot paths. Depends on LLVM. Separate deliverable.
- **LucidAOT** — compiles bytecode to native object files for shipping. May use LLVM or emit C and rely on the system C compiler. Separate deliverable.

Neither is part of the base product. Both are additions that leave the frontend and bytecode format unchanged. They will be documented separately when they exist.

### The layers

```
Source (.luc)
    │
    ▼
Lexer  ──►  Parser  ──►  Sema          (frontend — no LLVM, no host)
                              │
                              ▼
                      Bytecode Compiler  (AST → bytecode)
                              │
                              ▼
                       Bytecode Module   (the interchange format)
                              │
                              ▼
                        Interpreter      (executes the module)
                              │
                              ▼
                    Runtime Library + Host Registry
                    (C functions the interpreter calls)
```

The frontend produces a validated AST. The bytecode compiler lowers it to a bytecode module. The interpreter executes the module. Every stage downstream of the frontend is backend-replaceable: the bytecode module is the contract, and any future backend consumes the same module.

---

## 2. Execution Pipeline

A complete walk through what happens when the user runs `lucid run main.luc`:

**Step 1 — Module Resolution.** The `ModuleResolver` scans all source files referenced by `main.luc` (via import declarations), builds a dependency graph, checks for cyclic imports, and produces a topologically ordered list of modules to compile.

**Step 2 — Lexing.** The `Lexer` reads each source file character by character and produces a flat stream of `Token` values. Whitespace and comments are stripped. Keywords, identifiers, literals, and operators become typed tokens. The `TokenStream` gives the parser a buffered, lookahead-capable view of this stream.

**Step 3 — Parsing.** The `Parser` consumes the `TokenStream` and builds an Abstract Syntax Tree. The AST is allocated in an `ASTArena` — a bump-pointer memory pool that makes allocation fast and frees the entire tree in one operation. Identifiers are stored as interned strings via `StringPool` for O(1) equality comparison.

**Step 4 — Semantic Analysis.** The `Sema` pass walks the AST and validates it:
- `NameResolver` resolves every identifier to its declaration.
- `ScopeManager` enforces lexical scoping.
- `TypeChecker` infers and validates types, checks assignment compatibility, and validates call arguments.
- `FFIValidator` checks every `#host(...)` binding against the host registry.
- `TraitChecker` verifies trait conformance for every `satisfy` block.
- Linear-value rules for `Deferred<T>` are enforced here.

Errors are collected and reported via `Diagnostics`. No bytecode is generated until the AST is clean.

**Step 5 — Bytecode Compilation.** The bytecode compiler walks the validated AST and emits a single `BytecodeModule` for the whole program. It produces:
- One `FunctionProto` per function declaration (from every module).
- One `ModuleStateProto` per source module (the module-level variable slots).
- A `ProgramInitProto` and a `ProgramFreeProto`.
- A `Manifest` describing the program's host-facing interface (entry symbol, foreign library names, host symbols referenced).

The compiler's job is to lower AST constructs to opcodes. It does no type inference (Sema did that), no overload resolution (the resolved `DEF` is already stored on the AST), and no narrowing (Sema's decisions are already recorded on the AST). The compiler is small because Sema did the hard work.

**Step 6 — Execution.** The CLI hands the `BytecodeModule` to the `Interpreter`. The interpreter:
1. Loads any host functions or types the module references from the host registry (they were already registered when the host started).
2. Runs the program initializer (`__lucid_program_init`) to set up module-level state.
3. Invokes the entry point.
4. Returns the exit code.

The interpreter never touches source files or ASTs. Its input is a bytecode module and a host registry.

**Step 7 (optional) — Bytecode Serialization.** If the user passed `--emit-bytecode`, the CLI serializes the `BytecodeModule` to a `.lucb` file before execution. This is the only disk output the base product produces.

---

## 3. Frontend

The frontend is the entire pipeline from source text to validated AST. It is shared across all backends (the interpreter today; a future JIT or AOT backend). It has no dependency on the runtime library and no dependency on the host registry — Sema checks `#host(...)` bindings against a registry *interface*, but the registry itself is supplied by the caller.

### 3.1 Lexer

**Location:** `src/parser/lexer/`

The lexer transforms raw source text into a stream of tokens. It is a single pass over the input with no backtracking. Every `Token` carries:
- A `TokenType` enum value (keyword, identifier, literal, operator, etc.).
- A `SourceLocation` (file, line, column) for diagnostics.
- A string payload for identifiers and literals.

The `TokenStream` wraps the raw token stream and gives the parser:
- `peek(n)` — lookahead n tokens without consuming.
- `advance()` — consume and return the current token.
- `expect(kind)` — consume and assert the kind, or emit a diagnostic.

**Key files:**
- `Lexer.hpp/cpp` — the lexer itself.
- `TokenStream.hpp/cpp` — buffered stream with lookahead.

### 3.2 Parser

**Location:** `src/parser/`

The parser consumes the `TokenStream` and produces an AST. The grammar is encoded directly in the parsing functions — no grammar table, no generated parser.

The expression parser is a **Pratt parser** (top-down operator precedence): each operator has a binding power, and the parser recurses based on those powers.

`ParserContext` is the shared state threaded through all parsing functions: current token stream, arena allocator, string pool, diagnostic sink, parse flags.

**Key files:**
- `Parser.hpp/cpp` — public entry points: `parse()`, `parseFile()`.
- `ModuleResolver.hpp/cpp` — multi-file resolution, cycle detection.
- `context/ParserContext.hpp` — shared parse state.
- `context/TokenStream.hpp/cpp` — buffered stream with lookahead.
- `rules/ParseDecl.cpp` — `TYPE`, `FN`, `const`, `let`, `struct`, `enum`, `trait`, `satisfy`, `DEF`.
- `rules/ParseExpr.cpp` — Pratt parser for expressions.
- `rules/ParseStmt.cpp` — `if`, `for`, `while`, `switch`, `return`, blocks.
- `rules/ParseType.cpp` — type annotations (`[*]T`, `&T`, `T?`, generics, function types).
- `rules/ParseConcurrency.cpp` — `async`, `await`, `spawn`, `start`.
- `support/ErrorRecovery.hpp/cpp` — synchronisation points.
- `support/Helpers.cpp` — attribute parsing, doc-comment attachment.
- `support/LookAhead.cpp` — disambiguation helpers.

**AST node categories** (in `src/core/ast/`):
- `DeclAST` — declarations: functions, variables, structs, enums, traits, host types, aliases, DEFs, satisfies.
- `StmtAST` — statements: if, for, while, return, block, concurrency.
- `ExprAST` — expressions: binary, unary, call, index, field access, literals, pipelines, closures.
- `TypeAST` — type annotations: primitives, arrays, references, generics, function types.

### 3.3 Semantic Analysis

**Location:** `src/sema/`

Semantic analysis is a multi-pass walk over the AST. Passes run in order; each may annotate AST nodes with resolved types and declaration references. All errors are collected and reported together at the end.

**Passes in order:**

1. **NameResolver** — builds the declaration map for each scope. Resolves every identifier to the declaration it refers to. Detects use-before-declaration and undefined names.

2. **TypeChecker** — assigns a resolved type to every expression node. Validates assignments, function arguments, and return statements. Handles nullable (`T?`), fallible (`T!`), combined (`T?!`), reference (`&T`), and generic types. Enforces the flow-sensitive narrowing rules.

3. **TraitChecker** — checks every `satisfy` block against its trait: `FIELD` clauses against the target type's fields, `REQUIRE` clauses against the target's `DEF`s. Checks trait inheritance.

4. **FFIValidator** — checks every `#host(...)` binding against the host registry. The symbol must be registered; the declared signature must match the registered signature. Mismatches produce a diagnostic naming both signatures.

5. **LinearValueChecker** — enforces the `Deferred<T>` linear-value rules. A live `Deferred<T>` must be consumed exactly once (by `await` or `cancel`) on every control-flow path. A second consumption is a compile error.

6. **CaptureAnalysis** — computes the captured variables for every `AnonFuncExprAST` and assigns each capture a slot in the closure's environment.

**Key files:**
- `Sema.hpp/cpp` — entry point: `Sema::analyze(Module*)` runs all passes.
- `rules/SemaDecl.cpp`, `SemaExpr.cpp`, `SemaStmt.cpp` — per-node checks.
- `types/SemaType.hpp` — the semantic type representation.
- `types/SemaResolve.cpp` — `TypeAST` → semantic type.
- `types/SemaTypeEquality.cpp`, `SemaTypePredicates.cpp`, `SemaValidate.cpp` — type utilities.
- `context/SemaContext.hpp/cpp` — shared state.
- `context/ContextStack.hpp/cpp` — lexical scope stack.
- `context/Generic.hpp/cpp`, `Instantiation.cpp` — monomorphization.
- `const_eval/` — compile-time constant evaluation.
- `support/CaptureAnalysis.hpp/cpp`, `MangledName.hpp/cpp`, `Truthiness.hpp`, `TypeNarrowHelpers.hpp/cpp`, `SwitchHelpers.hpp/cpp`.
- `registry/AttributeValidator.hpp/cpp`, `IntrinsicValidator.hpp/cpp`, `ArgTypeValidators.hpp/cpp`.

---

## 4. Bytecode

**Location:** `src/bytecode/`

The bytecode compiler lowers the validated AST to a `BytecodeModule`. It is the only consumer of the AST besides the frontend, and the only producer of the interpreter's input.

### 4.1 What the compiler emits

**One `BytecodeModule` per program.** It contains:

- **One `FunctionProto` per function declaration.** A `FunctionProto` holds:
  - The function's mangled name.
  - Its parameter count and local slot count.
  - Its bytecode (`std::vector<Instruction>`).
  - Constant pool references.
  - Host-symbol references (`#host`, `#builtin`, `#native`).
  - Debug info (source locations per instruction, for diagnostics).

- **One `ModuleStateProto` per source module.** This is the storage for the module's top-level `let`/`const` bindings. It is a fixed-size array of slots; the bytecode for module-level access reads and writes slots by index.

- **A `ProgramInitProto`** — runs at load time, initializes every module's state slots to their declared initial values.

- **A `ProgramFreeProto`** — runs at teardown, releases every module's resources in reverse declaration order.

- **A `Manifest`** — the plain-data contract between the compiler and the interpreter:
  - `entry.symbol` — the mangled name of the program's entry point.
  - `programInitSymbol` / `programFreeSymbol` — always present.
  - `modules` — one entry per source module, with its state-slot count.
  - `hostSymbols` — the `#host`, `#builtin`, `#native` names the program references.
  - `foreignLibraries` — the `@[link("...")]` library names.

**Module-level access** is a slot-indexed read or write into the module's state array:

```
; bytecode for reading module-level `m.x`
LOAD_MODULE_STATE   <moduleIndex>
LOAD_SLOT           <slotIndex>
```

Two instructions per access. There is no null check — the state array exists from the moment the module loads.

### 4.2 The opcode set

The opcode set is deliberately small and JIT-friendly. It is register-based rather than stack-based, so that a future LucidJIT can map each opcode to a native instruction without a stack-simulation pass. It is designed to be stable across versions — adding opcodes is possible; changing their semantics is not.

**Categories:**

| Category            | Example opcodes                                                                           |
| ------------------- | ----------------------------------------------------------------------------------------- |
| Load / store        | `LOAD_CONST`, `LOAD_SLOT`, `STORE_SLOT`, `LOAD_LOCAL`, `STORE_LOCAL`, `LOAD_MODULE_STATE` |
| Arithmetic          | `ADD_I32`, `SUB_I32`, `MUL_I32`, `DIV_I32`, `MOD_I32`, `ADD_F32`, ...                     |
| Comparison          | `EQ_I32`, `LT_I32`, `LE_I32`, ...                                                         |
| Logical             | `LOGICAL_AND`, `LOGICAL_OR`, `LOGICAL_NOT` (with short-circuit variants)                  |
| Bitwise             | `BIT_AND`, `BIT_OR`, `BIT_XOR`, `SHL`, `SHR`, `BIT_NOT`                                   |
| Control flow        | `JUMP`, `JUMP_IF_FALSE`, `JUMP_IF_TRUE`, `JUMP_IF_NIL`, `JUMP_IF_ERR`                     |
| Calls               | `CALL_DIRECT`, `CALL_CLOSURE`, `CALL_HOST`, `CALL_BUILTIN`, `CALL_DEF`                    |
| Return              | `RETURN`, `RETURN_UNIT`                                                                   |
| Aggregates          | `STRUCT_NEW`, `ARRAY_NEW`, `FIELD_GET`, `FIELD_SET`, `INDEX_GET`, `INDEX_SET`             |
| Nullable / fallible | `NARROW_NIL`, `NARROW_ERR`, `WRAP_NILABLE`, `WRAP_FALLIBLE`, `COALESCE`                   |
| Closures            | `CLOSURE_NEW`, `CAPTURE_GET`, `CAPTURE_SET`                                               |
| Concurrency         | `SPAWN`, `START`, `AWAIT`, `CANCEL`                                                       |
| Trait dispatch      | `SATISFY_CALL` (dispatches through the resolved DEF table)                                |
| Host interface      | `HOST_CALL`, `HOST_TYPE_NEW`                                                              |

The full list lives in `src/bytecode/Opcode.hpp`. Adding an opcode is a change to the compiler and the interpreter; both must be updated together. The format is versioned so a `.lucb` produced by one version can be rejected by an incompatible interpreter.

### 4.3 The bytecode module format

A `BytecodeModule` is the in-memory representation. It is a flat structure with no pointers between sub-objects — everything is index-based, so the module can be serialized and deserialized without pointer fixups.

```cpp
struct BytecodeModule {
    std::vector<FunctionProto>   functions;
    std::vector<ModuleStateProto> modules;
    FunctionProto*               programInit;
    FunctionProto*               programFree;
    Manifest                     manifest;
    ConstantPool                 constants;
    HostSymbolTable              hostSymbols;
};
```

The constant pool holds every literal used by the module (integers, floats, strings, struct field names). Instructions refer to constants by index.

The host symbol table holds every `#host`, `#builtin`, `#native` name the module references. At load time, the interpreter resolves each name against the host registry and stores the resolved function pointer or type descriptor in the module's runtime state.

### 4.4 The `.lucb` archive

**Location:** the bytecode serializer in `src/bytecode/Serialize.hpp/cpp`.

A `.lucb` file is the serialized form of a `BytecodeModule`. It uses a simple binary format:

- A magic number (`LUCI`).
- A format version.
- The constant pool.
- The host symbol table (names only, not pointers).
- The module list.
- The function list.
- The manifest.
- Debug info (optional; stripped in release mode).

The format is target-independent — no alignment is assumed, no endianness is baked in. A `.lucb` produced on one machine can be read on another, as long as both runtimes support the same format version.

**See §9.2 for the archive's role in distribution.**

### 4.5 Key files

- `Bytecode.hpp/cpp` — the public entry point and `BytecodeModule`.
- `Opcode.hpp` — the opcode enum and instruction encoding.
- `FunctionProto.hpp/cpp` — the per-function prototype.
- `ModuleStateProto.hpp/cpp` — the per-module state description.
- `Manifest.hpp` — the plain-data contract.
- `ConstantPool.hpp/cpp` — literal interning.
- `HostSymbolTable.hpp/cpp` — host-name references.
- `Serialize.hpp/cpp` — the `.lucb` serializer.
- `compile/` — the compiler internals:
  - `Compiler.hpp/cpp` — the top-level compiler driver.
  - `EmitDecl.cpp`, `EmitStmt.cpp`, `EmitExpr.cpp`, `EmitPlace.cpp` — per-node lowering.
  - `Frame.hpp/cpp` — the compiler's view of a function frame (slots, scopes, temporaries).
  - `ConstantFolding.cpp` — folds literal arithmetic at compile time.

---

## 5. Interpreter

**Location:** `src/interp/`

The interpreter executes a `BytecodeModule`. It is the runtime's core: a dispatch loop, a value stack, a frame stack, and the interface to the runtime library and the host registry.

### 5.1 The execution loop

The interpreter's main loop is a classic opcode dispatch:

```
while (true) {
    op = code[pc++];
    switch (op) {
        case OP_LOAD_CONST:  { ... }
        case OP_ADD_I32:     { ... }
        case OP_CALL_HOST:   { ... }
        // ...
    }
}
```

Each instruction reads its operands from the instruction stream, operates on the interpreter's value slots, and advances the program counter.

The interpreter is **single-threaded**. Fibers are cooperative: the interpreter's own execution is one fiber at a time, and suspension points are the `AWAIT` opcode. There is no preemption, no OS threads, no data races on script data.

### 5.2 Values and the stack

Values are a tagged union:

```cpp
struct Value {
    enum class Tag { Int, Float, Bool, Char, String, Struct, Array, Closure, HostRef, Nil, Err } tag;
    union {
        int64_t      i;
        double       f;
        bool         b;
        char         c;
        InternedID   s;      // string handle
        StructRef    st;     // reference into the value arena
        ArrayRef     a;
        ClosureRef   cl;
        HostRef      h;      // opaque host value
    };
};
```

The interpreter uses a **register-style frame** rather than a value stack: each function has a fixed-size frame of slots, and opcodes read and write slots by index. This is why the bytecode is described as register-based in §4.2 — the ops are slot-indexed.

The frame layout for a function:

```
[ parameters | locals | temporaries | module-state pointer | closure-env pointer ]
```

The compiler assigns each variable a slot index; the interpreter allocates the frame once on entry and reuses it.

### 5.3 Frames and calls

The interpreter maintains a **frame stack**. Each frame holds:
- A pointer to the function's `FunctionProto`.
- A pointer to the frame's slots.
- The current program counter.
- A pointer to the caller's frame (for return).
- A pointer to the module state array (for module-level access).

Calls push a new frame; returns pop it. The `CALL_DIRECT` opcode uses a pre-computed slot offset; `CALL_CLOSURE` loads the closure's code pointer and environment from the closure value; `CALL_HOST` jumps to a registered C++ function pointer; `CALL_BUILTIN` jumps to a compiler-emitted handler.

### 5.4 The runtime library interface

The interpreter calls into the runtime library for anything that requires C++ work:
- String operations (`str_concat`, `str_slice`, `str_eq`, ...).
- Memory allocation (`alloc`, `free`).
- Closure environment management (`alloc_env`, `retain_env`, `release_env`).
- Concurrency (`spawn`, `start`, `await`, `cancel`).
- Map operations (`map_new`, `map_get`, `map_set`, ...).
- Panics (`panic_str`).
- Warnings (`warn_str`).

Each call is an ordinary C function call through a pointer resolved at load time. There is no marshaling layer; the ABI is defined in `src/runtime-abi/lucid_abi.h`.

### 5.5 Hot reload

The interpreter supports reloading a program without restarting the process. The workflow:

1. The CLI's `FileWatcher` detects a change.
2. The CLI re-parses, re-semas, and re-compiles the affected modules.
3. The CLI hands a fresh `BytecodeModule` to `Interpreter::reload`.
4. The interpreter:
   - Runs the old module's `programFree` to release resources.
   - Swaps in the new module.
   - Runs the new module's `programInit` to set up fresh state.
   - Resumes execution at the entry point (or at a designated safe point).

State does not survive a reload. The new program starts with fresh module state. Preserving state across a reload is a higher-tier feature and is not implemented.

### 5.6 Key files

- `Interpreter.hpp/cpp` — the public facade: `load`, `reload`, `run`, `close`.
- `Frame.hpp/cpp` — the frame representation.
- `Value.hpp/cpp` — the value representation.
- `Dispatch.cpp` — the main opcode loop.
- `Ops/` — per-opcode implementations, grouped by category:
  - `OpsLoadStore.cpp`, `OpsArithmetic.cpp`, `OpsComparison.cpp`, `OpsControl.cpp`, `OpsCall.cpp`, `OpsAggregate.cpp`, `OpsConcurrency.cpp`, `OpsHost.cpp`.
- `ExecutionResult.hpp` — exit code, timing, entry symbol.
- `InterpreterError.hpp` — the interpreter's exception type.

---

## 6. Runtime Library

**Location:** `src/runtime/` and `src/runtime-abi/`

The runtime library implements the C functions the interpreter calls into. It is written in C++ and links into the same binary as the interpreter. The ABI between the interpreter and the runtime is pinned in `src/runtime-abi/`.

### 6.1 The ABI table (`functions.def`)

**Location:** `src/runtime-abi/functions.def`

`functions.def` is the single source of truth for the runtime ABI. It is an X-macro table with one row per runtime function. Each row names the function's enumerator, its linker-level symbol name, its return type tag, and its parameter type tags.

The table has three consumers, all reading the same rows:

1. `src/bytecode/Abi.hpp` — emits the `RuntimeFn` enumerators used by the compiler.
2. `src/runtime-abi/lucid_runtime.h` — declares the `extern "C"` prototype of every row.
3. `src/runtime/exports.cpp` — takes the address of every row's function, so a row with no definition is a link error naming the symbol.

Because all three read the same table, their views cannot drift.

The ABI's struct layouts (`LucidString`, `LucidSlice`, `LucidMap`, `LucidClosure`, `LucidClosureHeader`, `LucidDeferred`) are pinned in `src/runtime-abi/lucid_abi.h`, with `static_assert`s on sizes, offsets, alignment, and triviality.

**Key files:**
- `runtime-abi/functions.def` — the X-macro table.
- `runtime-abi/lucid_abi.h` — the C-layout structs and named scalar types.
- `runtime-abi/lucid_runtime.h` — the `extern "C"` prototypes.

### 6.2 Runtime implementation

The runtime implements the functions declared by the table. It is written in C++ and links against the interpreter via the ABI contracts in `lucid_abi.h`.

- `StringRuntime.cpp` — string operations: `str_concat`, `str_slice`, `str_eq`, `str_from_ptr`, and the scalar formatters (`int_to_str`, `float_to_str`, `bool_to_str`, `char_to_str`, `uint_to_str`, `ptr_to_hex_str`).
- `MemoryRuntime.cpp` — `__lucid_alloc`, `__lucid_free`, the allocation registry, and `__lucid_leak_report`.
- `ClosureRuntime.cpp` — `__lucid_alloc_env`, `__lucid_retain_env`, `__lucid_release_env`.
- `ConcurrencyEntry.cpp`, `ConcurrencyRuntime.hpp/cpp` — `__lucid_spawn`, `__lucid_start`, `__lucid_await`, `__lucid_cancel`, `__lucid_shutdown_concurrency`, the fiber scheduler, and the completion queue.
- `PanicRuntime.cpp` — `__lucid_panic`.
- `ClosureEnvironment.hpp` — the internal closure environment layout, pinned against `LucidClosureHeader` in `lucid_abi.h`.
- `RuntimeInternal.hpp` — internal helpers shared across runtime translation units. Not part of the ABI; not prefixed with `__lucid_`.
- `exports.cpp` — takes the address of every runtime function, forcing the linker to check that each is defined.

Every `__lucid_*` symbol is a row in `functions.def`. Internal helpers live in namespace `lucid::runtime`, not under the `__lucid_` prefix.

---

## 7. Host Registry

**Location:** `src/host/`

The host registry is the interface between the language runtime and the application that embeds it. It holds the C++ functions and types the application wants to expose to Lucid code, and it is the table that `#host(...)` resolves against.

### 7.1 The registration model

The application registers its own functions and types with the runtime *before* any script loads. The runtime holds them in the registry; the compiler resolves `#host(...)` against the registry at compile time; the interpreter looks them up by index at load time.

Registration is explicit. The application calls:

```cpp
vm.register_function("my_game_function", (void*)&my_game_function, sig);
vm.register_type("MyGameType", type_info_of<MyGameType>());
```

The `Signature` carries the parameter and return types so the compiler can validate the Lucid-side declaration against the registered C++ function. The `TypeInfo` carries size, alignment, and refcount metadata for the registered type.

### 7.2 Compile-time validation

When the parser reaches `FN foo (...) -> ... = #host(my_game_function)`, Sema:

1. Looks up `"my_game_function"` in the registry. If not found, it's a compile error.
2. Compares the declared signature against the registered one. A mismatch is a compile error naming both.
3. Records a reference to the registry entry, which the bytecode compiler turns into a `HOST_CALL` reference.

The validation happens at compile time, in-process. There is no separate toolchain step, no header parsing, no linker search.

### 7.3 The embedding API

The public C++ API an application uses:

```cpp
namespace lucid {

class VM {
public:
    VM();
    ~VM();

    // Registration
    void register_function(const std::string& name, void* fn, const Signature& sig);
    void register_type(const std::string& name, const TypeInfo& info);

    // Loading
    void load_core_scripts();
    void load_source(const std::string& path);
    void load_bytecode(const std::string& path);        // .lucb
    void load_bytecode(const BytecodeModule& module);

    // Execution
    ExecutionResult run(const std::string& entry = "main");

    // Reload (for run mode)
    void reload();

    // Diagnostics
    DiagnosticEngine& diagnostics();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace lucid
```

An application that embeds Lucid links against `liblucid` and uses `lucid::VM`. It never sees the parser, the AST, the bytecode compiler, or the interpreter internals. The registration API and the run/reload API are the entire boundary.

**Key files:**
- `Registry.hpp/cpp` — the registry itself.
- `VM.hpp/cpp` — the embedding entry point.
- `TypeInfo.hpp` — the runtime type descriptor.
- `Signature.hpp/cpp` — the function-signature description.
- `bindings/BindFunction.hpp`, `BindMethod.hpp`, `BindStruct.hpp` — templated helpers for common registration patterns.
- `HostType.hpp` — a base tag for user-defined host types.

---

## 8. Standard Library

**Location:** `src/stdlib/`

The standard library is written in Lucid, not C++. Each module is a `.luc` file compiled alongside user code. Standard library modules are resolved before user modules in dependency order so their declarations are always available.

| Module            | Provides                                                           |
| ----------------- | ------------------------------------------------------------------ |
| `core.luc`        | Primitives, operators, `OpKind` declarations, `#builtin` bindings. |
| `core.map.luc`    | The `Map<K, V>` type and its operations.                           |
| `core.array.luc`  | Array operations and functional helpers.                           |
| `core.string.luc` | String manipulation.                                               |
| `core.math.luc`   | Arithmetic, trigonometry, random.                                  |
| `core.io.luc`     | Console and file I/O via the host VFS.                             |
| `core.fn.luc`     | Function composition helpers.                                      |
| `core.simd.luc`   | SIMD type aliases (`Float4`, `Int4`, ...) and operations.          |

The standard library uses `#host(...)` and `#builtin(...)` bindings to call into the runtime library's C functions. It presents clean Lucid APIs to users who never need to see the C boundary.

---

## 9. Distribution

### 9.1 File extensions

| Extension | What it is                                          | Produced by                     | Consumed by     |
| --------- | --------------------------------------------------- | ------------------------------- | --------------- |
| `.luc`    | Lucid source                                        | the developer                   | the frontend    |
| `.lucb`   | Lucid bytecode archive                              | `lucid compile --emit-bytecode` | the interpreter |
| `.luci`   | Library interface file (types, exported signatures) | not yet implemented             | Sema, LSP       |

### 9.2 The `.lucb` bytecode archive

A `.lucb` file is the serialized form of a `BytecodeModule`. It contains everything the interpreter needs to run the program:

- The constant pool.
- The host symbol table (names only).
- Every function proto.
- Every module state proto.
- The program init and free protos.
- The manifest.
- Optional debug info.

A `.lucb` file does **not** contain host function pointers or host type descriptors. Those are resolved against the host registry at load time, by name. This is deliberate: a `.lucb` is portable, and the host registry is the only thing that ties it to a particular embedding.

`.lucb` is versioned. The interpreter rejects a `.lucb` whose format version it does not support. The version is bumped only when the bytecode format changes incompatibly.

### 9.3 Whole-program module model

The bytecode compiler produces **one `BytecodeModule` for the entire program**, not one per source file. This is the same model the frontend already uses (one validated AST for the whole program), and it has the same reasons:

- **Cross-file references are resolved at compile time.** The compiler has already resolved every name, every `DEF`, every trait conformance. There is no cross-module linking step at load time.
- **Hot-reload granularity is the whole program.** Any source change triggers a full re-compile. Reload swaps the whole bytecode module.
- **One program init / free pair.** There is one initializer and one freer for the whole program, in dependency order.

Per-file hot reload would require splitting the bytecode module into per-file units and re-introducing cross-module symbol resolution. That was the previous design and was deliberately removed; the whole-program model is the design.

---

## 10. CLI and LSP

### 10.1 CLI

**Location:** `src/cli/`

Each command is a thin wrapper that drives the shared pipeline. `main.cpp` dispatches to the frontend commands (`run`, `parse`, `sema`) or, in the future, to the AOT backend.

**Command surface:**

```
lucid run   <file.luc>                          -- parse + sema + compile + interpret
lucid run   <file.lucb>                         -- load bytecode archive, interpret
lucid parse <file.luc> [--json|--json-pretty] [-o out]  -- stop after AST
lucid sema  <file.luc> [--json|--json-pretty] [-o out]  -- stop after semantic analysis
lucid compile <file.luc> [-o out.lucb]          -- compile to bytecode archive
lucid build <file.luc>                          -- NOT READY (requires LucidAOT backend)
lucid repl                                      -- NOT READY (interactive REPL)
```

**`lucid run`** is the primary command. It parses, sema's, compiles, and interprets. With `--no-hot-reload` off (the default), it also starts a `FileWatcher` in a background thread; when a watched file changes, the watcher re-runs the pipeline and calls `Interpreter::reload`.

**`lucid parse`** and **`lucid sema`** stop the pipeline early for tooling. They emit either a human-readable dump or JSON, controlled by flags.

**`lucid compile`** produces a `.lucb` file. It does not execute; it is a pure compile-to-bytecode step. Useful for shipping pre-compiled scripts.

**`lucid build`** is the future AOT command. It is recognized but not implemented; it will require the LucidAOT backend, which is not part of the base product.

**`lucid repl`** is a future interactive shell. Not implemented.

**Hot reload.** With `--no-hot-reload` off (the default), `lucid run <file.luc>` starts a `FileWatcher` in a background thread. When a watched file changes, the watcher re-parses, re-semas, and re-compiles the affected modules, then hands the new `BytecodeModule` to `Interpreter::reload`. The interpreter frees the old program's resources, installs the new module, and runs its program initializer (see §5.5).

The CLI owns file watching, module re-resolution, the re-compile cycle, and the decision of when to reload. The interpreter never sees a file path or an AST; it takes a `BytecodeModule` and makes it runnable.

**Key files:**
- `main.cpp` — CLI entry point.
- `CLIContext.hpp` — shared context for a single run session.
- `CLIOptions.hpp` — unified option parsing.
- `RunOptions.hpp` — options specific to `lucid run`.
- `DependencyGraph.hpp` — bi-directional import graph for hot reload.
- `FileWatcher.hpp` — filesystem watcher.
- `commands/run.hpp/cpp` — the `run` command.
- `commands/parse.hpp/cpp` — the `parse` command.
- `commands/sema.hpp/cpp` — the `sema` command.
- `commands/compile.hpp/cpp` — the `compile` command.
- `commands/build.hpp/cpp` — the `build` command (stub; marked NOT READY).
- `pipeline/Pipeline.hpp/cpp` — the compiler pipeline with configurable stop points.
- `pipeline/JSONDumper.hpp/cpp` — AST and diagnostic serialization.

> [!NOTE]
> **Implementation status.** `main.cpp` today implements `run`, `parse`, `sema`, and `compile`. `build` and `repl` are recognized but return a "not ready" diagnostic. The AOT backend (§not-yet-written) will be added in a future release; until then, `build` is a placeholder.

### 10.2 LSP

**Location:** `src/lsp/`

The server implements the Language Server Protocol so editors (VS Code, Neovim, etc.) can provide:
- Real-time diagnostics (errors and warnings as you type).
- Autocomplete for identifiers, fields, and type annotations.
- Go-to-definition and find-references.
- Hover documentation from doc-comments.

The LSP server reuses the lexer, parser, and semantic analysis passes on each file change. It runs as a separate process and communicates with the editor via stdin/stdout JSON-RPC. It never touches the bytecode compiler or the interpreter, and the `luc_langserver` binary is expected to link only the frontend (no interpreter, no runtime library).

---

## 11. File Structure

```
lucid/                                 # the language runtime (standalone, embeddable library + CLI)
├── README.md
├── LICENSE
├── CMakeLists.txt
├── .gitignore
├── .patches/
│
├── docs/
│   ├── grammar/
│   │   ├── LUCID_GRAMMAR.md            # the language specification
│   │   ├── CORE_SCRIPTS.md             # the standard core scripts
│   │   └── BUILTIN_REGISTRY.md         # the #builtin / #native names
│   ├── ARCHITECTURE.md                 # this document
│   ├── EMBEDDING.md                    # how an application embeds the runtime
│   ├── API.md                          # the public C++ API
│   ├── BUILD.md
│   └── examples/
│
├── temp/
└── src/
    │
    ├── main.cpp                        # the `lucid` CLI entry point
    │
    ├── core/                           # language-agnostic shared types (no runtime, no host)
    │   ├── SourceLocation.hpp
    │   ├── Tokens.hpp
    │   ├── ASTStrings.hpp
    │   ├── JSONFormatter.hpp/cpp
    │   ├── ast/                        # AST node definitions
    │   │   ├── BaseAST.hpp
    │   │   ├── DeclAST.hpp
    │   │   ├── ExprAST.hpp
    │   │   ├── StmtAST.hpp
    │   │   ├── TypeAST.hpp
    │   │   └── ResourceKind.hpp/cpp
    │   ├── memory/
    │   │   ├── ASTArena.hpp
    │   │   ├── ArenaSpan.hpp
    │   │   ├── InternedString.hpp
    │   │   └── StringPool.hpp/cpp
    │   ├── diagnostics/
    │   │   ├── DiagCode.hpp
    │   │   ├── Diagnostic.hpp/cpp
    │   │   └── StackTrace.hpp/cpp
    │   └── trace/
    │       └── Trace.hpp/cpp
    │
    ├── parser/                         # frontend stage 1 — source text → AST
    │   ├── Parser.hpp/cpp
    │   ├── ModuleResolver.hpp/cpp
    │   ├── lexer/
    │   │   └── Lexer.hpp/cpp
    │   ├── context/
    │   │   ├── ParserContext.hpp
    │   │   └── TokenStream.hpp/cpp
    │   ├── rules/
    │   │   ├── ParseDecl.cpp
    │   │   ├── ParseExpr.cpp
    │   │   ├── ParseStmt.cpp
    │   │   ├── ParseType.cpp
    │   │   └── ParseConcurrency.cpp
    │   └── support/
    │       ├── ErrorRecovery.hpp/cpp
    │       ├── Helpers.cpp
    │       └── LookAhead.cpp
    │
    ├── sema/                           # frontend stage 2 — AST → validated AST
    │   ├── Sema.hpp/cpp
    │   ├── context/
    │   │   ├── SemaContext.hpp/cpp
    │   │   ├── ContextStack.hpp/cpp
    │   │   ├── Generic.hpp/cpp
    │   │   └── Instantiation.cpp
    │   ├── rules/
    │   │   ├── SemaDecl.cpp
    │   │   ├── SemaExpr.cpp
    │   │   └── SemaStmt.cpp
    │   ├── types/
    │   │   ├── SemaType.hpp
    │   │   ├── SemaResolve.cpp
    │   │   ├── SemaTypeEquality.cpp
    │   │   ├── SemaTypePredicates.cpp
    │   │   └── SemaValidate.cpp
    │   ├── const_eval/
    │   │   ├── ConstEvaluator.hpp/cpp
    │   │   ├── ConstEvalBinary.cpp
    │   │   ├── ConstEvalUnary.cpp
    │   │   ├── ConstEvalStatement.cpp
    │   │   └── ConstEvalHelpers.hpp/cpp
    │   ├── registry/
    │   │   ├── AttributeValidator.hpp/cpp
    │   │   ├── IntrinsicValidator.hpp/cpp
    │   │   └── ArgTypeValidators.hpp/cpp
    │   └── support/
    │       ├── CaptureAnalysis.hpp/cpp
    │       ├── MangledName.hpp/cpp
    │       ├── Truthiness.hpp
    │       ├── TypeNarrowHelpers.hpp/cpp
    │       └── SwitchHelpers.hpp/cpp
    │
    ├── bytecode/                       # AST → bytecode module
    │   ├── Bytecode.hpp/cpp
    │   ├── Opcode.hpp
    │   ├── FunctionProto.hpp/cpp
    │   ├── ModuleStateProto.hpp/cpp
    │   ├── Manifest.hpp
    │   ├── ConstantPool.hpp/cpp
    │   ├── HostSymbolTable.hpp/cpp
    │   ├── Serialize.hpp/cpp           # the .lucb serializer
    │   └── compile/
    │       ├── Compiler.hpp/cpp
    │       ├── EmitDecl.cpp
    │       ├── EmitStmt.cpp
    │       ├── EmitExpr.cpp
    │       ├── EmitPlace.cpp
    │       ├── Frame.hpp/cpp
    │       └── ConstantFolding.cpp
    │
    ├── interp/                         # bytecode module → execution
    │   ├── Interpreter.hpp/cpp
    │   ├── Frame.hpp/cpp
    │   ├── Value.hpp/cpp
    │   ├── Dispatch.cpp
    │   ├── Ops/
    │   │   ├── OpsLoadStore.cpp
    │   │   ├── OpsArithmetic.cpp
    │   │   ├── OpsComparison.cpp
    │   │   ├── OpsControl.cpp
    │   │   ├── OpsCall.cpp
    │   │   ├── OpsAggregate.cpp
    │   │   ├── OpsConcurrency.cpp
    │   │   └── OpsHost.cpp
    │   ├── ExecutionResult.hpp
    │   └── InterpreterError.hpp
    │
    ├── runtime-abi/                    # the ABI surface shared by the compiler, runtime, and interpreter
    │   ├── functions.def
    │   ├── lucid_abi.h
    │   └── lucid_runtime.h
    │
    ├── runtime/                        # the runtime library implementation
    │   ├── StringRuntime.cpp
    │   ├── MemoryRuntime.cpp
    │   ├── ClosureRuntime.cpp
    │   ├── ConcurrencyRuntime.hpp/cpp
    │   ├── ConcurrencyEntry.cpp
    │   ├── PanicRuntime.cpp
    │   ├── RuntimeInternal.hpp
    │   ├── RuntimeError.hpp
    │   └── exports.cpp
    │
    ├── host/                           # the embedding API
    │   ├── Registry.hpp/cpp
    │   ├── VM.hpp/cpp
    │   ├── TypeInfo.hpp
    │   ├── Signature.hpp/cpp
    │   ├── HostType.hpp
    │   └── bindings/
    │       ├── BindFunction.hpp
    │       ├── BindMethod.hpp
    │       └── BindStruct.hpp
    │
    ├── stdlib/                         # the standard library (written in Lucid)
    │   ├── core.luc
    │   ├── core.map.luc
    │   ├── core.array.luc
    │   ├── core.string.luc
    │   ├── core.math.luc
    │   ├── core.io.luc
    │   ├── core.fn.luc
    │   └── core.simd.luc
    │
    ├── cli/
    │   ├── CLIContext.hpp
    │   ├── CLIOptions.hpp
    │   ├── RunOptions.hpp
    │   ├── DependencyGraph.hpp
    │   ├── FileWatcher.hpp
    │   ├── commands/
    │   │   ├── run.hpp/cpp
    │   │   ├── parse.hpp/cpp
    │   │   ├── sema.hpp/cpp
    │   │   ├── compile.hpp/cpp
    │   │   └── build.hpp/cpp           # stub — NOT READY
    │   └── pipeline/
    │       ├── Pipeline.hpp/cpp
    │       └── JSONDumper.hpp/cpp
    │
    └── debug/
        ├── DebugMacros.hpp
        └── DebugUtils.hpp

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
│   └── test_constraints.cpp
├── bytecode/
│   ├── test_compile.cpp
│   └── test_serialize.cpp
├── interp/
│   ├── test_dispatch.cpp
│   ├── test_reload.cpp
│   └── test_teardown.cpp
├── runtime/
│   ├── test_string.cpp
│   ├── test_memory.cpp
│   └── test_concurrency.cpp
├── host/
│   ├── test_registration.cpp
│   └── test_bindings.cpp
├── stdlib/
│   ├── test_io.luc
│   └── test_math.luc
└── integration/
    └── test_programs.luc
```

---

## Appendix — Pipeline at a glance

For the reader who wants the whole flow in one place:

```
Source file (.luc)
    │
    ▼
[CLI] ModuleResolver ──► topologically ordered module list
    │
    ▼
[CLI] Lexer ──────────► tokens
    │
    ▼
[CLI] Parser ─────────► AST
    │
    ▼
[CLI] Sema ───────────► validated AST
    │
    ▼
[CLI] bytecode::compile ───► BytecodeModule
                              (functions + module states
                               + manifest + host symbol table)
    │
    ├───────────────┬───────────────────┐
    ▼               ▼                   ▼
[CLI] Serialize  [CLI] Interpreter  [Tooling]
.lucb file       ::load(module)     (e.g. parse / sema dumps)
                 ::run()
                     │
                     ▼
              executed in-process
              (no file output unless --emit-bytecode)
```

**Ownership at each stage:**

| Stage             | Owner       | Input                    | Output              |
| ----------------- | ----------- | ------------------------ | ------------------- |
| Module resolution | CLI         | `main.luc` path          | ordered module list |
| Lexing            | CLI         | source text              | tokens              |
| Parsing           | CLI         | tokens                   | AST                 |
| Sema              | CLI         | AST                      | validated AST       |
| Bytecode compile  | CLI         | validated AST            | `BytecodeModule`    |
| Serialize         | CLI         | `BytecodeModule`         | `.lucb` file        |
| Interpreter load  | Interpreter | `BytecodeModule`         | internal state      |
| Interpreter run   | Interpreter | entry symbol in manifest | `ExecutionResult`   |
| File watching     | CLI         | filesystem               | change callbacks    |

The interpreter never appears in the left column of the frontend stages. That is the point of the design: the frontend produces a `BytecodeModule`, and the interpreter consumes it. Nothing in between.
</｜｜DSML｜｜ calls>