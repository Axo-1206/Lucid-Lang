# Luc Compiler – Code Generation Plan

> **Status:** Draft for implementation  
> **Last updated:** 2026‑05‑01  
> **Related docs:** `LUC_GRAMMAR.md`, `LUC_PROJECT_OVERVIEW.md`

This document describes the **code generation pipeline** for the Luc compiler, from AST to executable code (JIT or AOT). It includes:

- Dual compilation (JIT for development + shipping, AOT for native binaries)
- Async/await coroutines
- Parallel execution (`parallel for`, `parallel` block)
- Generics (monomorphization)
- **Type conversions (primitive and user‑defined `from` declarations)**
- Full LLVM backend integration

**Note on comments:** Luc supports line comments (`--`), block comments (`/- -/`), and doc comments (`/-- --/`). All comments are ignored during code generation. Doc comments are stored in the symbol table for tooling but do not affect IR.

---

## 1. Overall Architecture

```text
┌─────────────┐     ┌──────────────────┐     ┌─────────────┐
│  Luc Source │────►│ Semantic Analyser│────►│   CodeGen   │
└─────────────┘     └──────────────────┘     └──────┬──────┘
                                                     │
                                                     ▼
                              ┌──────────────────────────────────┐
                              │        CompilationDriver         │
                              │  (chooses JIT or AOT per @main)  │
                              └────────┬──────────────┬──────────┘
                                       │              │
                                       ▼              ▼
                           ┌──────────────┐    ┌──────────────┐
                           │  JIT Engine  │    │  AOT Compiler│
                           │  (ORCv2)     │    │  (MCJIT)     │
                           └──────┬───────┘    └──────┬───────┘
                                  │                   │
                                  │      .lbc bytecode│
                                  │      ┌────────────┘
                                  ▼      ▼
                           ┌──────────────────┐
                           │  Serializer /    │
                           │  Bytecode format │
                           └──────────────────┘
```

All code generation passes operate on LLVM IR. The same IR is used for JIT and AOT; only the final emission step differs.

---

## 2. Core Codegen Phases (Sequential)

### Phase 0: LLVM Context & Basic Setup
- Initialize `llvm::LLVMContext`, `llvm::Module`, `llvm::IRBuilder<>`.
- Set target triple and data layout.
- Define helper functions for type conversion (Luc → LLVM type).

### Phase 1: Expressions (Literals, Arithmetic, Variables)
- Literals, arithmetic, comparisons, variable access.
- Simple control flow for expression‑level `if` (ternary style).

### Phase 2: Statements & Control Flow
- Blocks, if‑else (statement), while, for‑range, return, break/continue.

### Phase 3: Functions & Non‑Generic Types
- Function declarations (single parameter group), calls, local `let`, structs (no generics), field access, type aliases, nullable types.

### Phase 3.5: Generics (Monomorphization)
**Syntax correction:** `impl Vec2<T> : Drawable` (not `impl<T> Vec2<T>`).
- Generic structs, functions, and impl blocks.
- Monomorphization algorithm with caching and mangling.
- Trait constraints.

### Phase 4: Impl Methods (Non‑Generic)
- `impl` blocks → regular functions.
- Method call `Type:method` and pipeline step desugaring.

### Phase 5: Type Conversions & Casting 
- **Primitive conversions:** `int(x)`, `float(x)`, `string(x)`, etc. → LLVM `sitofp`, `fptosi`, `uitofp`, `zext`, `trunc`, or runtime calls.
- **User‑defined conversions from `from` declarations:** each `from` entry becomes a function. Explicit `Target(expr)` and implicit casts (in assignments, argument passing, returns) generate a call to that function.
- **Implicit coercion:** inserted during expression codegen where semantic analysis requires it.
- **Nullable unwrapping:** produce runtime panic if `nil` is unwrapped.

New files: `ConversionBuiltin.cpp`, `ConversionUser.cpp`, `CoercionPass.cpp`.

### Phase 6: Arrays
- Fixed `[N]T`, slice `[]T`, dynamic `[*]T`. LLVM types and runtime calls for methods (`.push`, `.len`, etc.).

### Phase 7: Advanced Expressions & Operators
- Currying (multiple parameter groups), closures, match expressions, pipeline `->`, composition `+>`.

### Phase 8: Async / Await (Coroutines)
- Transform `async` functions using LLVM coroutine intrinsics.
- Runtime scheduler (`AsyncRuntime.cpp`).

### Phase 9: Parallel Execution
- `parallel for` → OpenMPIRBuilder or manual work‑stealing.
- `parallel` block → task parallelism with thread pool.

### Phase 10: Dual Compilation – JIT & AOT
- `CompilationDriver` unified interface.
- JIT: ORCv2, AOT: MCJIT + object file emission.
- Bytecode `.lbc` serialisation.

### Phase 11: FFI & Intrinsics
- `@extern`, `@sizeof`, `@alignof`, `@sqrt`, `@memcpy`, `@bitcast`, etc.

---

## 3. Integration Notes

### 3.1 Generic Impl Syntax
Correct:
```luc
impl Vec2<T> : Drawable { ... }
```

### 3.2 Interaction of Conversions with Other Features
- **Generics + conversions:** A generic `from` is not allowed; conversions must be concrete types. The monomorphizer will handle generic structs that appear in conversion signatures.
- **Async + conversions:** A conversion function can be `async` – it becomes a coroutine that returns a future of the target type.
- **Parallel + conversions:** No special handling.

### 3.3 Runtime Library
The runtime (`luc_runtime`) provides:
- Memory management for dynamic arrays, closures.
- Async scheduler.
- Thread pool for parallel blocks.
- Array built‑ins.
- **Conversion helpers** (e.g., `int_to_string`).

---

## 4. Implementation Order (Summary)

1. Phase 0–2 – Basic IR
2. Phase 3 – Functions & types (non‑generic)
3. Phase 3.5 – Generics
4. Phase 4 – Impl methods
5. Phase 5 – Conversions
6. Phase 6 – Arrays
7. Phase 7 – Advanced operators
8. Phase 8 – Async/await
9. Phase 9 – Parallel
10. Phase 10 – Dual compilation
11. Phase 11 – FFI

---

## 5. File Structure (New & Modified)

```
src/codegen/
├── CodeGen.cpp
├── CodeGenDecl.cpp
├── CodeGenExpr.cpp
├── CodeGenStmt.cpp
├── ValueEnv.hpp
├── GenericSpecialization.cpp    (NEW)
├── GenericCache.hpp
├── TypeSubstitution.cpp
├── ConversionBuiltin.cpp         (NEW)
├── ConversionUser.cpp            (NEW)
├── CoercionPass.cpp              (NEW)
├── CoroutineTransform.cpp
├── AsyncRuntime.cpp
├── ParallelTransform.cpp
├── CompilationDriver.cpp
├── AOTCompiler.cpp
├── JITEngine.cpp
└── BytecodeFormat.hpp

src/runtime/
├── luc_runtime.h
├── async_scheduler.cpp
├── task_pool.cpp
├── array_ops.cpp
├── conversion_helpers.cpp        (NEW – for int→string, etc.)
└── memory.cpp
```

---

## 6. Milestones & Success Criteria

- **M1 (Week 2):** Basic expressions + control flow.
- **M2 (Week 4):** Functions, structs, arrays.
- **M3 (Week 5):** Generics (monomorphization) working.
- **M4 (Week 6):** **Type conversions (primitive + user `from`) – can compile `Fahrenheit(Celsius{100})` and implicit casting.**
- **M5 (Week 8):** Async/await runs simple coroutines.
- **M6 (Week 10):** Parallel execution correct.
- **M7 (Week 12):** Dual compilation (JIT + AOT) passes tests.

**Final test:** the program from Phase 5 that uses both implicit and explicit conversions, plus all other features.

---

You can now save this as `CODEGEN.md`. If you need the raw text (without markdown formatting) for download, let me know and I’ll provide it.