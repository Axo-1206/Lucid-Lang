# Interpreter Phase — Architecture

> This document describes the internal architecture of the Lucid Interpreter phase: how validated AST modules are loaded, compiled JIT, and executed; how the major subsystems are structured; and how each file in the interpreter folder fits into that flow.

> [!NOTE]
> All code blocks here are `pseudo code` (or `cpp`), we use the `\```cpp` or `\```swift` for color effects, you will see some `.` or `,` or `;` at weird and inconsistent places, it's not typo but we keep them there so the color can be rendered.

## File Layout

```
src/interpreter/
├── Interpreter.hpp          # Public API - Orchestration
├── Interpreter.cpp          # Public API implementation (includes entry point finding)
│
├── core/
│   ├── InterpreterContext.hpp   # Central state container (header only)
│   └── ModuleRegistry.hpp/cpp   # Module tracking & dependencies
│
├── support/
│   ├── InterpreterOptions.hpp   # Configuration options
│   ├── InterpreterError.hpp     # Exception types & error kinds
│   ├── ExecutionResult.hpp      # Result of execution
│   └── PanicHandler.hpp/cpp     # Runtime panic handling
│
├── execution/
│   └── ModuleLoader.hpp/cpp     # Module loading & IR generation
│
├── jit/
│   ├── JITSession.hpp/cpp       # LLVM ORC JIT wrapper
│   └── JITCompiler.hpp/cpp      # IR compilation & module management
│
└── dynlink/
    ├── DynamicLinker.hpp/cpp    # Platform-agnostic library loader
    └── LibraryHandle.hpp/cpp    # RAII wrapper for library handles
```

**Removed Files:**
- `SymbolResolver.hpp/cpp` - Inlined into `Interpreter.cpp`
- `InterpreterContext.cpp` - Merged into header

---

## 1. Overview

The Interpreter phase is the runtime execution engine for the Lucid compiler. It takes semantically validated AST modules, lowers them to LLVM IR via the CodeGen phase, and executes them using LLVM's ORC (On-Request Compilation) JIT framework. The interpreter supports:

- **JIT Execution** — Compile and run Lucid code immediately (`lucid run`)
- **Hot-Reload** — Update running code without restarting
- **Foreign Library Loading** — Load and link dynamic libraries at runtime
- **Multi-Module Support** — Load modules in dependency order

```
                    ┌──────────────────┐
                    │  Validated AST   │
                    │ (from Sema)      │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  ModuleLoader    │  Load or reload modules
                    │  loadOrReload    │  - Validate modules
                    │  Modules()       │  - Register libraries
                    │                  │  - Generate names
                    │                  │  - Lower to IR (via CodeGen)
                    │                  │  - Add to JIT
                    │                  │  - Update Registry
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  Interpreter     │  Find entry point (inline)
                    │  findEntryPoint  │  - Scan loaded modules
                    │  (static)        │  - Check @[export] attribute
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  JITSession      │  Execute compiled code
                    │  lookupSymbol()  │  - Look up function in JIT
                    │                  │  - Call entry point
                    └──────────────────┘
```

---

## 2. Design Principles

### 2.1 No Version Tracking

The interpreter uses **module names as unique identifiers** — no version tracking is needed. Hot-reload replaces a module by removing the old one and adding the new one with the same name. Versioning belongs in version control (git), not in the runtime interpreter.

```cpp
// Simple ModuleInfo - no version field
struct ModuleInfo {
    InternedString name;                       // Unique identifier
    ModuleAST* ast = nullptr;                 // Current AST
    bool isActive = true;
    std::vector<InternedString> dependencies; // Modules this depends on
    std::set<InternedString> dependents;      // Modules that depend on this
};
```

### 2.2 Single Entry Point

The interpreter supports **only `main`** as the entry point. No fallback to `start` or `run`.

### 2.3 Inline Symbol Resolution

`SymbolResolver` has been removed — its functionality is inlined into `Interpreter.cpp` as static helpers.

---

## 3. InterpreterContext (`core/`)

The `InterpreterContext` is the central state container for the interpreter. It holds all shared state, dependencies, and module tracking information needed during interpretation.

### Architecture Overview

```swift
┌────────────────────────────────────────────────────────────────────────────┐
│                         InterpreterContext State Categories                │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  1. Resources (Input/Output)                                          │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  StringPool&          pool        │ Interned string storage           │ │
│  │  DiagnosticEngine&    diagnostics │ Error reporting                   │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  2. State & Configuration                                             │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  InterpreterOptions  options     │ Verbose, optimization, hot-reload  │ │
│  │  PanicHandler        panicHandler│ Runtime panic handling             │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  3. JIT & Dynamic Linking                                             │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  JITSession          jit         │ LLVM ORC JIT wrapper               │ │
│  │  DynamicLinker       linker      │ Foreign library loader             │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  4. Module Tracking                                                   │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  ModuleRegistry      moduleRegistry│ Tracks loaded modules            │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
└────────────────────────────────────────────────────────────────────────────┘
```

### Context Initialization Flow

```cpp
initialize(ctx, options)
│
├── if (ctx.jit.isInitialized()) return
│
├── ctx.options = options
│
├── ctx.jit.initialize()
│   ├── InitializeNativeTarget()
│   ├── InitializeNativeTargetAsmPrinter()
│   ├── InitializeNativeTargetAsmParser()
│   ├── setupTarget()          → Create LLJIT with host target
│   └── setupPlatformLibraries() → Load CRT/process symbols
│
└── if (verbose) print status
```

### Module Registry (`ModuleRegistry`)

The `ModuleRegistry` tracks all loaded modules and their dependency relationships.

**ModuleInfo Structure:**
```cpp
struct ModuleInfo {
    InternedString name;                    // Unique module name
    ModuleAST* ast = nullptr;              // AST (for re-compilation on hot-reload)
    bool isActive = true;
    std::vector<InternedString> dependencies;  // Modules this depends on
    std::set<InternedString> dependents;       // Modules that depend on this
    
    bool dependsOn(InternedString other) const;
    bool isDependencyOf(InternedString other) const;
};
```

**Registry Operations:**

| Method                        | Description                                          |
| ----------------------------- | ---------------------------------------------------- |
| `registerModule(name, ast)`   | Register or update a module (same name = update)     |
| `unregisterModule(name)`      | Remove a module and clean up dependencies            |
| `setDependencies(name, deps)` | Set dependency graph and validate for cycles         |
| `getAffectedModules(name)`    | BFS find all modules that depend on a changed module |
| `getActiveModule()`           | Get the currently active module                      |

**Hot-Reload Flow (No Version Tracking):**
```cpp
hotReloadModule(ctx, module, name)
│
├── if (ctx.jit.hasModule(name))
│   └── ctx.jit.removeModule(name)      // Remove old version
│
├── ctx.jit.addModule(irModule, name)   // Add new version (same name)
│
└── ctx.moduleRegistry.registerModule(name, module)  // Update registry
```

**Affected Modules Calculation:**
```cpp
getAffectedModules(changedModule)
│
├── BFS traversal starting from changedModule
│
├── Queue: [changedModule]
│   └── while queue not empty:
│       ├── current = queue.pop()
│       ├── for each dependent in current.dependents:
│       │   ├── if not visited:
│       │   │   ├── mark visited
│       │   │   ├── add to results
│       │   │   └── queue.push(dependent)
│       └──
│
└── return results (all transitive dependents)
```

### Invariants

| Invariant           | Description                                                 |
| ------------------- | ----------------------------------------------------------- |
| **Module Non-Null** | All module pointers in registry must be valid               |
| **Unique Names**    | Each module has a unique name (InternedString id)           |
| **No Cycles**       | Dependency graph must be acyclic (validated on each update) |
| **Single Active**   | Exactly one module is active at a time                      |
| **JIT Initialized** | JIT must be initialized before loading modules              |

---

## 4. Module Loading (`execution/`)

The `ModuleLoader` is responsible for loading or reloading AST modules into the interpreter. It handles the full pipeline from AST to executable code.

### Load/Reload Overview

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Module Loading/Reloading Flow                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  loadOrReloadModules(ctx, modules, isHotReload)                             │
│  │                                                                          │
│  ├── 1. Validate Modules                                                    │
│  │   └── Check each module is non-null and has no errors                    │
│  │                                                                          │
│  ├── 2. Register Foreign Libraries                                          │
│  │   └── ctx.linker.registerLibrariesFromModules()                          │
│  │                                                                          │
│  ├── 3. Generate Module Names                                               │
│  │   └── generateModuleName(ctx, module) → InternedString                   │
│  │                                                                          │
│  ├── 4. Extract Dependencies                                                │
│  │   └── extractModuleDependencies(ctx, module) → vector<InternedString>    │
│  │                                                                          │
│  ├── 5. Lower Modules to LLVM IR                                            │
│  │   └── lowerModulesSeparately(ctx, modules) → vector<unique_ptr<Module>>  │
│  │                                                                          │
│  ├── 6. Load or Reload Each Module                                          │
│  │   │                                                                      │
│  │   ├── If isHotReload:                                                    │
│  │   │   ├── if (ctx.jit.hasModule(name)) ctx.jit.removeModule(name)        │
│  │   │   ├── ctx.jit.addModule(irModule, name)                              │
│  │   │   └── ctx.moduleRegistry.registerModule(name, module)                │
│  │   │                                                                      │
│  │   └── If initial load:                                                   │
│  │       ├── ctx.jit.addModule(irModule, name)                              │
│  │       └── ctx.moduleRegistry.registerModule(name, module)                │
│  │                                                                          │
│  └── 7. Set Active Module                                                   │
│      └── ctx.moduleRegistry.setActiveModule(moduleNames[0])                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Module Name Generation

```cpp
generateModuleName(ctx, module)
│
├── if (module->filePath.isValid())
│   ├── name = pool.lookup(module->filePath)
│   ├── replace('/', '_')
│   ├── replace('\\', '_')
│   ├── replace('.', '_')
│   └── return pool.intern(name)
│
└── else
    └── return pool.intern("module_" + ptr)
```

### Hot-Reload Flow

```cpp
hotReloadModule(ctx, module, name)
│
├── 1. Validate
│   ├── module != null
│   ├── !module->hasErrors
│   └── ctx.options.enableHotReload
│
├── 2. Get affected modules (dependents)
│   └── affected = ctx.moduleRegistry.getAffectedModules(name)
│
├── 3. Build reload list
│   └── modulesToReload = [module] + affected.ast
│
├── 4. Reload all affected modules
│   └── loadOrReloadModules(ctx, modulesToReload, true)
│
└── 5. Return success
```

### Hot-Reload Example

```
Initial Load:
  ├── math.luc → math
  ├── array.luc → array
  └── main.luc → main
      └── depends on: math, array

File Change: math.luc modified

Hot-Reload:
  1. getAffectedModules("math") → [main] (main depends on math)
  2. modulesToReload = [math, main]
  3. loadOrReloadModules([math, main], true)
     ├── math: remove old → add new (same name)
     └── main: remove old → add new (recompiled with new math)
```

---

## 5. Entry Point Resolution (Inline in `Interpreter.cpp`)

Entry point finding is implemented as static helper functions directly in `Interpreter.cpp`.

### Entry Point Resolution Flow

```cpp
static bool isFunctionExported(FuncDeclAST* func, ctx)
│
├── exportName = pool.intern("export")
│
└── for each attr in func->attributes:
    └── if (attr->name == exportName) return true
    └── return false

static InternedString findEntryPoint(ctx, entryPoint)
│
├── 1. If specific entry point requested:
│   ├── for each module in ctx.moduleRegistry.getAllModules():
│   │   └── for each decl in module->decls:
│   │       └── if (FuncDeclAST && func->name == entryPoint)
│   │           └── if (isFunctionExported(func, ctx))
│   │               └── return entryPoint
│   └── return empty
│
└── 2. No specific entry point → look for "main"
    └── return findEntryPoint(ctx, pool.intern("main"))
```

### Why SymbolResolver Was Removed

The `SymbolResolver` was removed because:
1. **Only one function was actually used** — `findEntryPoint(InternedString)`
2. **The rest were dead code** — never called anywhere
3. **Sema already validates** — export checking is done by Sema's `AttributeValidator`
4. **Mangling is CodeGen's responsibility** — not needed in the interpreter

---

## 6. JIT Execution (`jit/`)

The JIT subsystem wraps LLVM's ORC (On-Request Compilation) JIT framework.

### JITSession Architecture

```swift
┌─────────────────────────────────────────────────────────────────────────────┐
│                         JITSession Overview                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  JITSession (Wrapper around LLVM ORC LLJIT)                                 │
│  │                                                                          │
│  ├── Resources:                                                             │
│  │   ├── StringPool&        m_stringPool                                    │
│  │   ├── LLVMContext        m_context (shared)                              │
│  │   └── LLJIT              m_jit (ORC JIT instance)                        │
│  │                                                                          │
│  ├── Module Management:                                                     │
│  │   ├── ResourceTrackerSP  m_trackers (name → tracker)                     │
│  │   ├── addModule(module, name)  → Compile & add IR module                 │
│  │   ├── removeModule(name)       → Remove module from JIT                  │
│  │   └── hasModule(name)          → Check if module is loaded               │
│  │                                                                          │
│  └── Symbol Lookup:                                                         │
│      └── lookupSymbol(name) → void* (function pointer)                      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Module Addition Flow

```cpp
addModule(module, name)
│
├── 1. Verify module
│   └── if (!module) throw error
│
├── 2. Verify IR
│   └── if (verifyModule(*module)) throw error
│
├── 3. Set target triple and data layout
│   ├── module->setTargetTriple(m_jit->getTargetTriple().str())
│   └── module->setDataLayout(m_jit->getDataLayout())
│
├── 4. Create ThreadSafeModule
│   ├── ctx = make_unique<LLVMContext>()
│   └── threadSafeModule = ThreadSafeModule(module, ctx)
│
├── 5. Add to JIT
│   ├── tracker = m_jit->getMainJITDylib().createResourceTracker()
│   ├── m_jit->addIRModule(tracker, threadSafeModule)
│   └── m_trackers[name.id] = tracker
│
└── 6. Store tracker
```

### Symbol Lookup Flow

```cpp
lookupSymbol(name)
│
├── if (!m_initialized) return nullptr
│
├── symbol = m_jit->lookup(name)
│
├── if (!symbol) return nullptr
│
└── return reinterpret_cast<void*>((*symbol).getValue())
```

### Platform Libraries Setup

```cpp
setupPlatformLibraries()
│
├── 1. Load CRT/System libraries
│   └── llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr)
│
├── 2. Create symbol generator for current process
│   └── Generator = DynamicLibrarySearchGenerator::GetForCurrentProcess()
│
├── 3. Add generator to JIT
│   └── m_jit->getMainJITDylib().addGenerator(std::move(Generator))
│
└── 4. Now all process symbols (malloc, free, printf, etc.) are available
```

### JITCompiler (Thin Wrapper)

The `JITCompiler` is a lightweight wrapper around `JITSession`.

```cpp
JITCompiler
│
├── compile(module, name)
│   ├── if (!module) throw InterpreterError
│   ├── if (m_session.hasModule(name)) m_session.removeModule(name)
│   └── m_session.addModule(std::move(module), name)
│
├── remove(name)
│   └── m_session.removeModule(name)
│
├── isLoaded(name)
│   └── m_session.hasModule(name)
│
└── lookup(name)
    └── m_session.lookupSymbol(name)
```

---

## 7. Dynamic Linking (`dynlink/`)

The `DynamicLinker` manages loading of foreign libraries and symbol resolution.

### Library Loading Flow

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Dynamic Linking Overview                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  User Code: @[link("opengl")]                                               │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  DynamicLinker::registerLibrariesFromModule()                         │  │
│  │  │                                                                    │  │
│  │  └── Scan module for @[link] attributes                               │  │
│  │      └── For each: linker.load(libName)                               │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                    │                                        │
│                                    ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  DynamicLinker::load(name)                                            │  │
│  │  │                                                                    │  │
│  │  ├── getLibraryPath(name) → "libopengl.so" / "opengl.dll"             │  │
│  │  ├── LibraryHandle handle(path) → RAII wrapper                        │  │
│  │  │   └── Platform-specific: LoadLibraryA() / dlopen()                 │  │
│  │  ├── m_libraries[name] = std::move(handle)                            │  │
│  │  └── m_cacheDirty = true                                              │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                    │                                        │
│                                    ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  DynamicLinker::registerWithJIT(jit)                                  │  │
│  │  │                                                                    │  │
│  │  └── for each library: jit.registerLibrarySymbols(path, name)         │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                    │                                        │
│                                    ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  JIT resolves foreign function calls                                  │  │
│  │                                                                       │  │
│  │  Lucid: @[foreign("C")] glClearColor(...)                             │  │
│  │  │                                                                    │  │
│  │  └── JIT lookup "glClearColor" → finds in loaded library              │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Platform Abstraction

```cpp
LibraryHandle
│
├── Windows:
│   ├── LoadLibraryA(path) → HMODULE
│   ├── GetProcAddress(handle, name) → FARPROC
│   └── FreeLibrary(handle)
│
└── Unix (Linux/macOS):
    ├── dlopen(path, RTLD_NOW | RTLD_GLOBAL) → void*
    ├── dlsym(handle, name) → void*
    └── dlclose(handle)
```

### Circular Dependency Resolution

The `DynamicLinker` does **not** depend on `InterpreterContext`. Instead, it takes explicit dependencies:

```cpp
// DynamicLinker uses explicit dependencies - no circular reference
void DynamicLinker::registerLibrariesFromModules(
    DiagnosticEngine& diagnostics,
    StringPool& pool,
    bool verbose,
    const std::vector<ModuleAST*>& modules
);
```

This avoids the circular dependency where `InterpreterContext` owns `DynamicLinker` and `DynamicLinker` needs `InterpreterContext`.

---

## 8. Error Handling (`support/`)

### Error Types

The interpreter uses a two-layer error system:

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Error System Overview                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  InterpreterErrorKind (Compile-time errors)                           │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  • InitFailed          • JIT initialization failed                    │  │
│  │  • ModuleLoadFailed    • Failed to compile module                     │  │
│  │  • EntryPointNotFound  • 'main' not found                             │  │
│  │  • HotReloadFailed     • Hot-reload operation failed                  │  │
│  │  • LibraryLoadFailed   • Foreign library not found                    │  │
│  │  • SymbolLookupFailed  • Symbol not found in JIT                      │  │
│  │  • EmptyModuleList     • No modules provided                          │  │
│  │  • InvalidIR           • LLVM IR verification failed                  │  │
│  │  • JITError            • JIT compilation error                        │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  RuntimeErrorKind (Runtime panics in compiled code)                   │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  • DivisionByZero      • ArrayIndexOutOfBounds                        │  │
│  │  • NullPointerDereference • DanglingPointer                           │  │
│  │  • DoubleFree          • AllocationFailed                             │  │
│  │  • UnwrappedNil        • TagMismatch                                  │  │
│  │  • ForeignCallFailed   • FutureAlreadyConsumed                        │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Error Flow

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Error Flow                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  1. Error occurs in interpreter                                             │
│     │                                                                       │
│     ├── Report to DiagnosticEngine with DiagCode                            │
│     │   └── diagnostics.error(DiagCode::Sem_UndefinedValue, ...)            │
│     │                                                                       │
│     └── Throw InterpreterError with InterpreterErrorKind                    │
│         └── throw InterpreterError(InterpreterErrorKind::InitFailed, msg)   │
│                                                                             │
│  2. Runtime panic in compiled code                                          │
│     │                                                                       │
│     ├── emitPanic(kind) in CodeGen                                          │
│     │   └── Calls __lucid_panic with message and DiagCode                   │
│     │                                                                       │
│     └── PanicHandler catches exception                                      │
│         ├── if (callback) callback(message, exitCode)                       │
│         └── else print to stderr                                            │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### PanicHandler

```cpp
PanicHandler
│
├── setCallback(callback)
│   └── Install custom panic handler (e.g., for GUI or logging)
│
├── handle(message)
│   ├── if (m_panicking) → fatal recursive panic
│   ├── m_panicking = true
│   ├── m_lastMessage = message
│   ├── if (callback) callback(message, 1)
│   ├── else print to stderr
│   ├── m_panicking = false
│   └── return 1
│
└── handle(exception)
    └── handle(exception.what())
```

---

## 9. Execution Pipeline (`Interpreter.cpp`)

### Complete Execution Flow

```cpp
runModules(ctx, modules, entryPoint, isHotReload)
│
├── 1. Initialize JIT (if not already)
│   └── if (!ctx.jit.isInitialized()) ctx.jit.initialize()
│
├── 2. Validate modules
│   └── for each module:
│       ├── if (null) throw ModuleLoadFailed
│       └── if (module->hasErrors) return error
│
├── 3. Load or reload modules
│   └── if (!loadOrReloadModules(ctx, modules, isHotReload))
│       └── return error
│
├── 4. Find entry point (inline)
│   ├── if (!entryPoint.isValid()) entryPoint = pool.intern("main")
│   ├── found = findEntryPoint(ctx, entryPoint)
│   ├── if (!found.isValid()) throw EntryPointNotFound
│   └── entryName = pool.lookup(found)
│
├── 5. Look up entry point symbol
│   ├── fnPtr = ctx.jit.lookupSymbol(entryName)
│   ├── if (!fnPtr) throw SymbolLookupFailed
│   └── mainFn = reinterpret_cast<int(*)()>(fnPtr)
│
├── 6. Execute entry point
│   ├── try:
│   │   ├── exitCode = mainFn()
│   │   └── result.success = true
│   ├── catch (const std::exception& e):
│   │   ├── exitCode = ctx.panicHandler.handle(e)
│   │   └── result.success = false
│   └──
│
├── 7. Report results
│   ├── result.exitCode = exitCode
│   ├── result.executionTimeMs = duration
│   └── result.entryPointUsed = entryName
│
└── 8. Return result
```

### Initial Run vs Hot-Reload

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                     Initial Run vs Hot-Reload                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Initial Run (isHotReload = false):                                         │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  • Load all modules fresh                                             │  │
│  │  • Register dependencies                                              │  │
│  │  • Set active module                                                  │  │
│  │  • Execute entry point                                                │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  Hot-Reload (isHotReload = true):                                           │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  • Find all affected modules (dependents)                             │  │
│  │  • Remove old versions from JIT (by name)                             │  │
│  │  • Add new versions to JIT (same name)                                │  │
│  │  • Update registry                                                    │  │
│  │  • Set active module                                                  │  │
│  │  • Next function call uses new version                                │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 10. Dependencies & API Reference

### Interpreter Public API (`Interpreter.hpp`)

| Method                                              | Description                          |
| --------------------------------------------------- | ------------------------------------ |
| `initialize(ctx, options)`                          | Initialize JIT and interpreter state |
| `runModules(ctx, modules, entryPoint, isHotReload)` | Load and execute modules             |
| `runModule(ctx, module, entryPoint, isHotReload)`   | Load and execute a single module     |
| `hotReloadModule(ctx, module, name)`                | Hot-reload a module and dependents   |
| `getLoadedModules(ctx)`                             | Get all loaded modules (ModuleInfo)  |
| `getAffectedModules(ctx, changedModule)`            | Get modules affected by a change     |

### Module Registry API (`ModuleRegistry`)

| Method                        | Description                         |
| ----------------------------- | ----------------------------------- |
| `registerModule(name, ast)`   | Register or update a module         |
| `unregisterModule(name)`      | Remove a module from the registry   |
| `getModuleInfo(name)`         | Get ModuleInfo for a module         |
| `hasModule(name)`             | Check if a module is registered     |
| `setDependencies(name, deps)` | Set dependencies and validate graph |
| `getAffectedModules(name)`    | Get all transitive dependents       |
| `getActiveModule()`           | Get the active module               |

### JIT Session API (`JITSession`)

| Method                               | Description                           |
| ------------------------------------ | ------------------------------------- |
| `initialize()`                       | Initialize JIT with host target       |
| `addModule(module, name)`            | Compile and add IR module to JIT      |
| `removeModule(name)`                 | Remove module from JIT                |
| `hasModule(name)`                    | Check if module is loaded             |
| `lookupSymbol(name)`                 | Look up symbol in JIT (returns void*) |
| `registerLibrarySymbols(path, name)` | Register dynamic library symbols      |
| `getContext()`                       | Get LLVMContext (for CodeGen)         |

### Dynamic Linker API (`DynamicLinker`)

| Method                                       | Description                        |
| -------------------------------------------- | ---------------------------------- |
| `load(name)`                                 | Load library by name               |
| `loadPath(path)`                             | Load library from path             |
| `unload(name)`                               | Unload library                     |
| `getSymbol(name)`                            | Get symbol from loaded libraries   |
| `registerLibrariesFromModule(ctx, module)`   | Scan module for @[link] attributes |
| `registerLibrariesFromModules(ctx, modules)` | Scan multiple modules              |
| `registerWithJIT(jit)`                       | Register all libraries with JIT    |
| `isLoaded(name)`                             | Check if library is loaded         |

---

## 11. File Dependencies

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         File Dependency Graph                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Interpreter.hpp                                                            │
│  │                                                                          │
│  ├── InterpreterContext.hpp                                                 │
│  │   ├── ModuleRegistry.hpp                                                 │
│  │   ├── JITSession.hpp                                                     │
│  │   ├── DynamicLinker.hpp                                                  │
│  │   ├── PanicHandler.hpp                                                   │
│  │   └── InterpreterOptions.hpp                                             │
│  │                                                                          │
│  └── ExecutionResult.hpp                                                    │
│      │                                                                      │
│      └── ModuleLoader.hpp (implementation)                                  │
│          ├── CodeGen.hpp (LLVM IR generation)                               │
│          └── DynamicLinker.hpp                                              │
│                                                                             │
│  JITSession.hpp                                                             │
│  ├── llvm/ExecutionEngine/Orc/LLJIT.h                                       │
│  └── llvm/IR/Module.h                                                       │
│                                                                             │
│  DynamicLinker.hpp                                                          │
│  ├── LibraryHandle.hpp                                                      │
│  └── JITSession.hpp                                                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Dependency Direction

```
Interpreter.hpp (Public API)
    │
    ▼
ModuleLoader.hpp → CodeGen.hpp (IR generation)
    │               │
    │               ▼
    │           LLVM (llvm::Module)
    │
    ▼
JITSession.hpp → LLVM ORC (llvm::orc::LLJIT)
    │
    ▼
DynamicLinker.hpp → LibraryHandle.hpp (Platform APIs)
```

**Key Principles:**
1. `Interpreter.hpp` depends on specialized modules, not the other way around
2. Each module has a single, well-defined responsibility
3. No circular dependencies (DynamicLinker takes explicit dependencies)
4. Symbol resolution is inlined (no separate SymbolResolver file)