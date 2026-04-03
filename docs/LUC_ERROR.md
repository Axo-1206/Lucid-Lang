# Luc — `error` Library Reference

> **Scope of this file:** Standard library documentation for the `error` package.
> The `error` package provides the `Error` struct, `ErrorCode` enum, and the `Expect<T>` 
> type alias.

---

## Philosophy

Errors in Luc are **values**, not exceptions. A function that can fail returns
an `Expect<T>` — either the success value `T` or an `Error`. Nothing unwinds,
no hidden control flow, no global state. 

### The Three Tiers of Error

| Tier | Category | Handled By | Behavior |
|---|---|---|---|
| **1** | **Compile Error** | Compiler | **Blocks compilation.** Syntax/Type errors. |
| **2** | **Expected Error** | Developer | **`Expect<T>`.** Must be handled or it blocks thread. |
| **3** | **Panic** | Runtime | **Unrecoverable.** Immediate thread-blocking. |

---

## Core Types

### `Error`

```luc
pub struct Error {
    message string
    code    int = 0
    cause   Error? = nil    -- chain of causes, nullable
}
```

### `Expect<T>`

```luc
pub type Expect<T> = T | Error
```

An `Expect<T>` is either a value of type `T` or an `Error`. Functions that can
fail return `Expect<T>` instead of `T` alone.

---

## Handling Errors

### 1. Fallback with `??`

The `??` operator Extracts `T` from an `Expect<T>` or returns a fallback value.

```luc
let value float = divide(10.0)(0.0) ?? 0.0
let cfg Config  = readConfig("app.config") ?? Config {}
```

### 2. Pipeline `catch`

The `catch` keyword is a specialized pipeline step that intercepts an `Error`.

```luc
let cfg Config = readFile("app.cfg")
    -> parseConfig
    -> catch((e Error) Config {
        io.printl("failed to load: " + e.message)
        return Config {}
    })
```

### 3. Forced Handling (Runtime Blocking)

To ensure safety, **any `Expect<T>` return value cannot be discarded.** If you call 
a function that returns an `Expect<T>` without using `??`, `catch`, or variable 
assignment, the compiler will generate a runtime check.

If the function returns an `Error` and it was discarded, the thread will **block 
immediately** with a Panic.

---

## Compiler Behaviour — Unhandled Results

| Situation | Compiler response |
|---|---|
| `Expect<T>` value is discarded | **Runtime Check** — Panics if result is `Error`. |
| `Expect<T>` used directly as `T` | **Compile Error** — Type mismatch. |

```luc
divide(10.0)(0.0)              -- runtime panic if result is Error

let x float = divide(10.0)(2.0)    -- ERROR: Expect<float> is not float
let x float = divide(10.0)(2.0) ?? 0.0  -- OK: ?? resolves to float
```