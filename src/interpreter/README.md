# Interpreter Backend

> **Location:** `src/interpreter/`

The interpreter backend is the final stage of the Lucid compiler pipeline. It takes a fully validated and annotated `ModuleAST` from the frontend, lowers it to LLVM IR, JIT-compiles it to native machine code using LLVM's ORC framework, and executes it immediately.

No object files are written to disk — compilation happens entirely in memory.

---

## Table of Contents

- [Overview](#overview)
- [Pipeline Flow](#pipeline-flow)
- [Components](#components)
  - [JITSession](#jitsession)
  - [DynLink](#dynlink)
  - [Interpreter](#interpreter)
- [Hot-Reload Support](#hot-reload-support)
- [Error Handling](#error-handling)
- [File Structure](#file-structure)
- [Build Dependencies](#build-dependencies)

---

## Overview

The interpreter is the **execution engine** for `lucid run`. It receives a validated AST from the frontend and executes it without producing any output files.

```
[ModuleAST] → [IRLowering] → [LLVM IR] → [ORC JIT] → [Native Code] → [Execution]
     ↑              ↑              ↑            ↑            ↑
  Validated      Shared with   IR Builder   LLJIT      Runtime
  AST            AOT backend   (codegen)    Session    Support
```

### Key Characteristics

- **In-Memory Compilation:** No object files, no linker invocation
- **Immediate Execution:** Functions are callable as soon as they're compiled
- **Hot-Reload:** Source changes can be recompiled and swapped at runtime
- **Foreign Symbol Resolution:** Automatic loading of `@[link]` libraries

---

## Pipeline Flow

### Step 1: Receive ModuleAST

After semantic analysis completes successfully, the interpreter receives a fully annotated `ModuleAST`:

```cpp
ModuleAST {
    filePath: "main.luc"
    decls: [
        // Each declaration has:
        // - resolved types (ExprAST::resolvedType)
        // - resolved declarations (IdentifierExprAST points to DeclAST)
        // - type-checked and validated
    ]
    errors: []  // Empty - semantic analysis passed
    hasErrors: false
}
```

**Key Annotations on AST Nodes:**

| Node Type           | Annotation                  | Purpose                             |
| ------------------- | --------------------------- | ----------------------------------- |
| `ExprAST`           | `resolvedType: TypeAST*`    | The resolved type of the expression |
| `IdentifierExprAST` | Resolved to `ValueDeclAST*` | Which declaration this refers to    |
| `VarDeclAST`        | `valueType: TypeAST*`       | The variable's resolved type        |
| `FuncDeclAST`       | `funcType: FuncTypeAST*`    | The function's full type            |

### Step 2: Register Foreign Libraries

The interpreter scans all declarations for `@[link("...")]` attributes and loads the corresponding shared libraries:

```lucid
@[foreign("C"), link("opengl")]
const glClear (mask uint32) = {}
```

The interpreter will:
1. Find `@[link("opengl")]`
2. Load `opengl.dll` (Windows) or `libopengl.so` (Linux)
3. Register all OpenGL symbols with the JIT's symbol table

### Step 3: Lower AST to LLVM IR

The `IRLowering` class walks the annotated AST and produces LLVM IR:

| Lucid Construct                   | LLVM IR Output                              |
| --------------------------------- | ------------------------------------------- |
| `const add (a int)(b int) -> int` | `define i32 @add(i32 %a, i32 %b)`           |
| `let x int = 42`                  | `%x = alloca i32` + `store i32 42, i32* %x` |
| `x + y`                           | `%add = add i32 %x, %y`                     |
| `@[foreign("C")] glClear`         | `declare void @glClear(i32)`                |
| `#sqrt(x)`                        | `call float @llvm.sqrt.f32(float %x)`       |

**Example: Lowering a Simple Function**

```lucid
const add (a int)(b int) -> int = {
    return a + b
}
```

Becomes:

```llvm
define i32 @add(i32 %a, i32 %b) {
entry:
    %add = add i32 %a, %b
    ret i32 %add
}
```

### Step 4: JIT Compile to Machine Code

The `JITSession` takes the LLVM IR module and compiles it to native machine code:

1. **ORC JIT** receives the LLVM IR
2. **Optimization Passes** run (if enabled)
3. **Code Generation** produces native machine code
4. **Machine code** is stored in executable memory
5. **Symbols** are registered in the JIT's symbol table

### Step 5: Execute

After JIT compilation, functions can be looked up and called:

```cpp
// Look up the entry point
auto* fnPtr = jit.lookupSymbol("main");

// Cast to function pointer and call
auto main = reinterpret_cast<int(*)()>(fnPtr);
return main();  // ← Program executes here!
```

---

## Components

### JITSession

**Location:** `JIT.hpp` / `JIT.cpp`

**Responsibility:** Wraps LLVM's ORC JIT API.

```cpp
class JITSession {
    // Owns:
    // - llvm::LLVMContext (shared with IRLowering)
    // - llvm::orc::LLJIT (the JIT engine)
    // - ResourceTrackers for hot-reload
    
    bool initialize();                          // Setup target, platform libs
    bool addModule(std::unique_ptr<llvm::Module>, const std::string& name);
    void removeModule(const std::string& name);
    bool reloadModule(std::unique_ptr<llvm::Module>, const std::string& name);
    void* lookupSymbol(const std::string& name);
};
```

**Key Behaviors:**

| Behavior              | Description                                                                               |
| --------------------- | ----------------------------------------------------------------------------------------- |
| **Initialization**    | Sets up the host target, creates the JIT instance, and registers platform libraries       |
| **Module Management** | Each module is added with a `ResourceTracker` that enables safe removal during hot-reload |
| **Symbol Lookup**     | Finds symbols in the JIT's symbol table (compiled Lucid functions + foreign symbols)      |
| **Hot-Reload**        | Removes old modules and adds new versions while the program runs                          |

**What JITSession Does NOT Do:**
- No IR generation (that's in `IRLowering`)
- No library loading (that's in `DynLink`)

---

### DynLink

**Location:** `DynLink.hpp` / `DynLink.cpp`

**Responsibility:** Platform-agnostic loading of foreign shared libraries.

```cpp
class DynLink {
    // Owns:
    // - Loaded library handles (HMODULE on Windows, void* on POSIX)
    // - Symbol maps for each loaded library
    
    bool load(const std::string& libraryName);      // By name (e.g., "m", "opengl")
    bool loadPath(const std::string& path);         // By full path
    void* getSymbol(const std::string& symbolName);
    void registerWithJIT(JITSession& jit, const std::string& libraryName);
};
```

**Foreign Symbol Resolution Flow:**

| Step | Action                                                                                                                   |
| ---- | ------------------------------------------------------------------------------------------------------------------------ |
| 1    | During IR Lowering: `@[foreign("C")]` declarations become LLVM `declare` statements                                      |
| 2    | During JIT Setup: For every `@[link("libname")]` annotation, `DynLink::load(libname)` calls `dlopen` / `LoadLibrary`     |
| 3    | All exported symbols are extracted and cached                                                                            |
| 4    | During Execution: When JIT-compiled code calls a foreign function, the JIT resolves the symbol from registered libraries |

**Platform Support:**

| Platform | Library Extension | Load Function | Get Symbol       |
| -------- | ----------------- | ------------- | ---------------- |
| Windows  | `.dll`            | `LoadLibrary` | `GetProcAddress` |
| Linux    | `.so`             | `dlopen`      | `dlsym`          |
| macOS    | `.dylib`          | `dlopen`      | `dlsym`          |

---

### Interpreter

**Location:** `Interpreter.hpp` / `Interpreter.cpp`

**Responsibility:** Orchestrates the entire interpreter pipeline.

```cpp
class Interpreter {
    // Owns:
    // - JITSession (the JIT engine)
    // - DynLink (foreign library loader)
    // - IRLowering (AST → LLVM IR)
    // - Module version tracking for hot-reload
    
    bool initialize();
    int runModule(ModuleAST* module, const std::string& entryPoint = "main");
    bool loadModule(ModuleAST* module);
    bool hotReload(ModuleAST* module, const std::string& moduleName);
    void registerForeignLibraries(ModuleAST* module);
    template<typename Ret, typename... Args> Ret execute(const std::string& name, Args... args);
};
```

**Lifecycle of `runModule`:**

```
1. Pre-flight check (ensure no frontend errors)
   ↓
2. Collect foreign libraries from @[link] attributes
   ↓
3. Lower AST to LLVM IR (via IRLowering)
   ↓
4. Add IR module to JIT session
   ↓
5. Look up entry point symbol
   ↓
6. Execute entry point (catches runtime panics)
   ↓
7. Return exit code
```

---

## Hot-Reload Support

The interpreter supports **hot-reloading** modules when source files change:

```cpp
bool Interpreter::hotReload(ModuleAST* module, const std::string& moduleName) {
    // 1. Generate new IR with versioned name
    auto versionedName = generateVersionedName(moduleName);
    auto irModule = m_irLowering->lower(module, versionedName);
    
    // 2. Remove old version from JIT
    auto it = m_moduleVersionMap.find(moduleName);
    if (it != m_moduleVersionMap.end()) {
        m_jit.removeModule(it->second);
    }
    
    // 3. Add new version
    if (!m_jit.addModule(std::move(irModule), versionedName)) {
        return false;
    }
    
    // 4. Update version tracking
    m_moduleVersionMap[moduleName] = versionedName;
    return true;
}
```

**How Hot-Reload Works:**

| Step | Action                                                            |
| ---- | ----------------------------------------------------------------- |
| 1    | File watcher detects source change                                |
| 2    | Re-runs lexer → parser → sema → IR lowering on the changed module |
| 3    | New IR module is added under a new version key                    |
| 4    | Old module is removed using its `ResourceTracker`                 |
| 5    | The next function call resolves to the new version                |

**Why This Works:**
- All game state lives in `luc_kernel`'s ECS (not inside Lucid functions)
- The hot-swapped module picks up exactly where the old one left off

---

## Error Handling

### Error Boundaries

| Phase          | Errors Handled                           | What Happens            |
| -------------- | ---------------------------------------- | ----------------------- |
| **Lexer**      | Invalid characters, unterminated strings | Reports error, stops    |
| **Parser**     | Syntax errors, missing semicolons        | Reports error, stops    |
| **Semantic**   | Type mismatches, undefined variables     | Reports error, stops    |
| **IRLowering** | Should never fail (validated AST)        | If fails, fatal error   |
| **JIT**        | Module add failures, symbol not found    | Throws JITError         |
| **Runtime**    | Division by zero, out-of-bounds          | Throws `runtime::Panic` |

### Error Handling Matrix

| Error Type            | Where It Happens             | Who Detects | Who Handles                           |
| --------------------- | ---------------------------- | ----------- | ------------------------------------- |
| Syntax error          | `Parser::parseFile()`        | Parser      | Frontend (reports, no IR)             |
| Semantic error        | `Sema::analyze()`            | Sema        | Frontend (reports, no IR)             |
| IR generation failure | `IRLowering::lower()`        | IRLowering  | Interpreter (fatal, shouldn't happen) |
| JIT add failure       | `JITSession::addModule()`    | JITSession  | Interpreter (fatal)                   |
| Symbol not found      | `JITSession::lookupSymbol()` | JITSession  | Interpreter (fatal)                   |
| Library not found     | `DynLink::load()`            | DynLink     | Interpreter (fatal)                   |
| Runtime panic         | `runtime::Panic`             | Runtime     | Interpreter (reports, returns error)  |

### JITError Types

```cpp
enum class JITError::Kind {
    InitFailed,          // JIT initialization failed
    ModuleAddFailed,     // Failed to add module to JIT
    ModuleRemoveFailed,  // Failed to remove module
    SymbolNotFound,      // Requested symbol not found
    LookupFailed,        // Symbol lookup operation failed
};
```

---

## File Structure

```
src/interpreter/
├── JIT.hpp              # ORC JIT session management
├── JIT.cpp              # JIT implementation
├── DynLink.hpp          # Dynamic library loader
├── DynLink.cpp          # DynLink implementation
├── Interpreter.hpp      # Main interpreter engine
├── Interpreter.cpp      # Interpreter implementation
└── README.md            # This file
```

### Dependencies

```
Interpreter
    ├── JITSession (LLVM ORC)
    │   └── llvm::orc::LLJIT
    │
    ├── DynLink (Platform)
    │   ├── dlopen / LoadLibrary
    │   └── dlsym / GetProcAddress
    │
    └── IRLowering (from src/compiler/)
        ├── llvm::IRBuilder
        ├── llvm::Module
        └── TypeMapping
```

---

## Build Dependencies

### Required Libraries

| Library      | Purpose                                 |
| ------------ | --------------------------------------- |
| **LLVM 18+** | ORC JIT, IR generation, code generation |
| **ZLIB**     | Required by LLVM's support library      |

### CMake Configuration

```cmake
# Find LLVM
find_package(LLVM REQUIRED CONFIG)

# Map LLVM components
llvm_map_components_to_libnames(llvm_libs 
    core 
    support 
    native 
    mc 
    target 
    orcjit 
    executionengine 
)

# Link everything
target_link_libraries(lucid-comp PRIVATE ${llvm_libs} ZLIB::ZLIB)
```

---

## Quick Reference

### Building and Running

```bash
# Build the interpreter
cmake -B build -S .
cmake --build build

# Run a Lucid program
./build/lucid-comp run main.luc

# Run with debug output
./build/lucid-comp run main.luc --debug
```

### Key APIs

| Operation      | Method                                  |
| -------------- | --------------------------------------- |
| Initialize JIT | `JITSession::initialize()`              |
| Add module     | `JITSession::addModule(module, name)`   |
| Remove module  | `JITSession::removeModule(name)`        |
| Lookup symbol  | `JITSession::lookupSymbol(name)`        |
| Load library   | `DynLink::load(name)`                   |
| Run program    | `Interpreter::runModule(module, entry)` |
| Hot-reload     | `Interpreter::hotReload(module, name)`  |

---

## Related Documentation

- [Architecture.md](../../docs/ARCHITECTURE.md) - Overall compiler architecture
- [Grammar.md](../../docs/GRAMMAR.md) - Lucid language grammar
- [IRLowering.hpp](../compiler/IRLowering.hpp) - AST → LLVM IR lowering
- [TypeMapping.hpp](../compiler/TypeMapping.hpp) - Lucid → LLVM type mapping
- [Runtime](../runtime/) - Runtime support (panics, memory, threading)
