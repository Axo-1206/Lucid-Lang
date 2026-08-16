# Code Generation Phase — Architecture

> This document describes the internal architecture of the Lucid Code Generation phase: how validated AST is lowered to LLVM IR, how the major subsystems are structured, and how each file in the codegen folder fits into that flow.

> [!NOTE]
> All code block here are `pseudo code` (or `cpp`), we use the `\```cpp` or `\```swift` for color effects

## File Layout

```
src/codegen/
├── CodeGen.hpp          # Public API
├── CodeGen.cpp          # Orchestrator
├── CodeGenType.hpp/cpp  # Lucid → LLVM type mapping
├── CodeGenDecl.cpp      # Declaration lowering
├── CodeGenStmt.cpp      # Statement lowering
├── CodeGenExpr.cpp      # Expression lowering
│
├── context/
│   └── CodeGenContext.hpp # LLVM state (module, builder, caches, symbols)
│
├── generic/
│   ├── CodeGenGeneric.hpp/cpp      # Detection + Substitution + Instantiation
│   ├── GenericSubstitution.hpp     # Type substitution context
│   └── GenericMangledName.hpp/cpp  # Generic mangled name generation
│
├── support/
│   ├── RuntimeError.hpp            # Define all runtime errors
│   ├── LLVMHelpers.hpp             # Work with llvm types, values
│   ├── CodeGenHelpers.hpp/cpp      # General helpers
│   ├── CodeGenAlloca.hpp/cpp       # Alloca, blocks, loads
│   └── CodeGenPanic.hpp/cpp        # Panic, null checks
│
├── closure/
│   └── CodeGenClosure.hpp/cpp      # Closure lowering
│
└── intrinsic/
    ├── IntrinsicEmitter.hpp             # Intrinsic emission API
    ├── IntrinsicEmitter.cpp             # Dispatcher
    ├── LLVMIntrinsicEmitter.hpp/cpp     # LLVM intrinsic emissions
    └── LucidIntrinsicEmitter.hpp/cpp    # Lucid intrinsic emissions
```

---

## 1. Overview

The CodeGen phase is the final stage of the Lucid frontend. It walks the semantically validated AST and emits LLVM IR for each module. CodeGen **trusts** the AST — all validation is done by Sema. If Sema succeeded, the AST is guaranteed to be well-formed. CodeGen should NOT validate or report semantic errors; it should only generate IR. Assertions are used only in debug builds to catch bugs in Sema.

```cpp
                    ┌──────────────────┐
                    │  Validated AST   │
                    │ (from Sema)      │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │   CodeGen.cpp    │  Orchestrator
                    │  generate()      │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  Phase 1:        │
                    │  Declarations    │  Lower all types, function prototypes,
                    │                  │  globals, structs. NO function bodies.
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  Phase 2:        │
                    │  Function Bodies │  Lower all function bodies with full
                    │                  │  symbol resolution (forward refs work)
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │  Verify Module   │  llvm::verifyModule()
                    └──────────────────┘
```

---

## 2. Execution Pipeline

A complete walk through what happens when CodeGen processes a module:

**Step 1 — Module Creation**
`generate()` creates a `CodeGenContext` for each module and instantiates an `llvm::Module` with the module's file path as its name.

**Step 2 — Phase 1: Lower Declarations**
`lowerModuleDeclarations()` walks all top-level declarations and:
- Creates `llvm::Function` prototypes for all function declarations (no bodies)
- Creates LLVM struct types for all struct declarations
- Creates LLVM integer constants for enum variants
- Creates global variables for module-level variables
- **Does NOT** generate function bodies

This phase enables forward references — a function can be called before it is defined because its prototype already exists.

**Step 3 — Phase 2: Lower Function Bodies**
`lowerModuleBodies()` walks all function declarations and:
- Creates entry blocks for each function
- Lower all parameters (create allocas, store arguments)
- Generate IR for the function body statements and expressions
- Verify the function with `llvm::verifyFunction()`

**Step 4 — Module Verification**
`llvm::verifyModule()` validates the generated IR. If verification fails, a diagnostic is reported and `nullptr` is returned.

```cpp
┌────────────────────────────────────────────────────────────────────────────────┐
│                         CodeGen Pipeline Walkthrough                           │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │  Step 1: generate()                                                     │   │
│  │  ├── For each module AST:                                               │   │
│  │  │   ├── ctx = CodeGenContext(p, d, context)                            │   │
│  │  │   ├── ctx.module = new llvm::Module(name, context)                   │   │
│  │  │   └── generateModule(module, ctx)                                    │   │
│  │  └── Return vector<unique_ptr<llvm::Module>>                            │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │  Step 2: generateModule()                                               │   │
│  │  ├── Phase 1: lowerModuleDeclarations(module, ctx)                      │   │
│  │  │   └── For each decl: lowerDeclaration(decl, ctx)                     │   │
│  │  │       ├── FuncDecl → lowerFunctionDecl() → Create prototype          │   │
│  │  │       ├── StructDecl → lowerStructDecl() → Create LLVM struct        │   │
│  │  │       ├── EnumDecl → lowerEnumDecl() → Create integer constants      │   │
│  │  │       └── VarDecl → lowerVarDecl() → Create global variable          │   │
│  │  ├── Phase 2: lowerModuleBodies(module, ctx)                            │   │
│  │  │   └── For each FuncDecl: lowerFunctionBody(decl, ctx)                │   │
│  │  │       ├── lowerFunctionBodyInternal() → Create entry block           │   │
│  │  │       ├── lowerParam() for each parameter → Create allocas           │   │
│  │  │       ├── lowerStatement(body) → Generate IR                         │   │
│  │  │       └── llvm::verifyFunction() → Validate function                 │   │
│  │  └── llvm::verifyModule() → Validate module                             │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Core Architecture

### 3.1 Two-Phase Function Lowering

Lucid supports forward references — a function can be called before it is defined. To enable this, function lowering is split into two passes:

| Phase       | What Happens                                                                   | Why                                                                                          |
| ----------- | ------------------------------------------------------------------------------ | -------------------------------------------------------------------------------------------- |
| **Phase 1** | Create `llvm::Function` prototypes for all functions. No bodies are generated. | Function prototypes are available for call resolution even before the function body is seen. |
| **Phase 2** | Generate the actual IR for each function body.                                 | By now, all symbols are declared and can be resolved.                                        |

```cpp
// Phase 1: Only prototypes
lowerFunctionDecl(FuncDeclAST* decl, ctx) {
    // Create llvm::Function with ExternalLinkage
    // No body - function is empty
    ctx.storeFunction(decl, func);
}

// Phase 2: Bodies
lowerFunctionBody(FuncDeclAST* decl, ctx) {
    llvm::Function* func = ctx.lookupFunction(decl);
    // Create entry block, lower parameters, generate body
    lowerFunctionBodyInternal(decl, func, ctx);
}
```

### 3.2 CodeGenContext

The `CodeGenContext` is the central state container for the code generation phase. It holds all LLVM-related state, symbol tables, caches, and control flow information needed during IR lowering.

#### 3.2.1 State Categories

```swift
┌────────────────────────────────────────────────────────────────────────────┐
│                         CodeGenContext State Categories                    │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  1. Resources (Input/Output)                                          │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  StringPool&          pool        │ Interned string storage           │ │
│  │  DiagnosticEngine&    diagnostics │ Error reporting                   │ │
│  │  llvm::LLVMContext&   llvmCtx     │ LLVM context (types, etc.)        │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  2. LLVM IR State                                                     │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  llvm::Module*       module      │ Current LLVM module                │ │
│  │  llvm::IRBuilder<>   builder     │ Instruction builder                │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  3. Type Caches                                                       │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  typeCache    │ TypeAST* → llvm::Type*    │ Type mapping cache        │ │
│  │  structCache  │ StructDeclAST* → StructType*│ Struct type cache       │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  4. Symbol Tables                                                     │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  values     │ ValueDeclAST* → llvm::Value*    │ Variables             │ │
│  │  functions  │ FuncDeclAST* → llvm::Function*  │ Functions             │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  5. Control Flow Context                                              │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  loops          │ Vector<LoopInfo>     │ Break/continue stack         │ │
│  │  currentFunction│ llvm::Function*      │ Current function             │ │
│  │  returnBlock    │ llvm::BasicBlock*    │ Return merge block           │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  6. Closure State                                                     │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  currentEnvPtr  │ llvm::Value*    │ Environment pointer               │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  7. Runtime Functions (Cached)                                        │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  runtimeFunctions │ string → llvm::Function* │ Runtime helpers        │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │  8. Generic Registry                                                  │ │
│  ├───────────────────────────────────────────────────────────────────────┤ │
│  │  genericRegistry │ GenericRegistry    │ Generic instantiations        │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                            │
└────────────────────────────────────────────────────────────────────────────┘
```

#### 3.2.2 Type Cache Lookup Flow

```cpp
getType(ctx, type, subst) {
    │
    ├── if (subst == nullptr) {
    │   └── Check typeCache for type → if found, return cached type
    │   }
    │
    ├── // Not in cache (or substitution present) — compute type
    │
    ├── switch (type->kind) {
    │   case PrimitiveType:  return getPrimitiveType(...)
    │   case NamedType:      return getNamedType(...)
    │   case ArrayType:      return getArrayType(...)
    │   case NullableType:   return getNullableType(...)
    │   case RefType:        return getRefType(...)
    │   case PtrType:        return getPtrType(...)
    │   case FuncType:       return getFunctionType(...)
    │   // ...
    │   }
    │
    └── if (result != nullptr && subst == nullptr) {
            typeCache[type] = result  // Cache only when no substitution
        }
        return result
}
```

**Why Generic Substitutions Are NOT Cached:**

When a generic substitution is present, the same `TypeAST*` with different type arguments yields different LLVM types. Substituted types are cached via the `GenericRegistry` using `GenericInstantiationKey`.

#### 3.2.3 Symbol Tables

```cpp
values: ValueDeclAST* → llvm::Value*
├── Key: VarDeclAST for "count"  → Value: llvm::AllocaInst* (local variable)
├── Key: VarDeclAST for "PI"     → Value: llvm::GlobalVariable* (global constant)
└── Key: ParamAST for "x"        → Value: llvm::AllocaInst* (parameter)

functions: FuncDeclAST* → llvm::Function*
├── Key: FuncDeclAST for "add"   → Value: llvm::Function* (i32 (i32, i32))
└── Key: FuncDeclAST for "main"  → Value: llvm::Function* (i32 ())
```

**Why Parameters Use Allocas:**

| Reason              | Explanation                                                                             |
| ------------------- | --------------------------------------------------------------------------------------- |
| **L-Value Support** | Parameters can be referenced as l-values (e.g., `&x`), which requires a memory location |
| **Mutability**      | Parameters can be mutable (if declared with `let`), requiring `load`/`store`            |
| **Consistency**     | Provides a uniform way to access all variables (both parameters and locals)             |

```cpp
lowerParam(ParamAST* param, ctx) {
    // 1. Get the LLVM type
    llvm::Type* paramType = getType(ctx, param->type);

    // 2. Get the argument value from the function
    llvm::Value* argValue = func->getArg(paramIndex);

    // 3. Create alloca
    llvm::AllocaInst* alloca = createAlloca(paramName, paramType, ctx);

    // 4. Store argument into alloca
    ctx.builder.CreateStore(argValue, alloca);

    // 5. Store in symbol table (mapped to the alloca, not the argument)
    ctx.storeValue(param, alloca);
    param->llvmAlloca = alloca;
    param->llvmValue = argValue;
}
```

#### 3.2.4 Control Flow Context

```cpp
LoopInfo Structure:
struct LoopInfo {
    llvm::BasicBlock* header;          // Loop condition check
    llvm::BasicBlock* exit;            // Exit on condition false
    llvm::BasicBlock* continueTarget;  // Continue (increment) point
};

Loop Stack Operations:
├── pushLoop(header, exit, continueTarget)  → pushes onto loop stack
├── popLoop()                                → pops innermost loop
└── currentLoop()                            → returns innermost loop context

Break/Continue Resolution:
├── lowerBreakStmt()     → builder.CreateBr(loop->exit)
└── lowerContinueStmt()  → builder.CreateBr(loop->continueTarget)
```

#### 3.2.5 Runtime Function Cache

```cpp
runtimeFunctions: string → llvm::Function*
├── "__lucid_async"   → llvm::Function* (void (ptr, ptr, ptr))
├── "__lucid_await"   → llvm::Function* (void (ptr))
├── "__lucid_spawn"   → llvm::Function* (void (ptr, ptr))
└── "__lucid_join"    → llvm::Function* (void (ptr))

getOrCreateRuntimeFunction(name, type) {
    // Check cache first
    if (func = getRuntimeFunction(name)) return func;
    // Not in cache - create new function
    func = llvm::Function::Create(type, ExternalLinkage, name, module);
    setRuntimeFunction(name, func);
    return func;
}
```

#### 3.2.6 Generic Registry

```cpp
GenericInstantiationKey:
struct GenericInstantiationKey {
    DeclAST* decl;                    // The generic declaration
    std::vector<TypeAST*> typeArgs;   // Concrete type arguments
};

GenericRegistry:
struct GenericRegistry {
    // Function instantiations: FuncDecl* → (key → Function*)
    std::unordered_map<FuncDeclAST*,
        std::unordered_map<GenericInstantiationKey, llvm::Function*>
    > functionInstantiations;

    // Struct instantiations: StructDecl* → (key → Type*)
    std::unordered_map<StructDeclAST*,
        std::unordered_map<GenericInstantiationKey, llvm::Type*>
    > structInstantiations;
};
```

#### 3.2.7 String Type and Literal Creation

```cpp
String Type Layout:
struct string {
    char*  data;   // Pointer to UTF-8 data (field 0)
    i64    len;    // Length in bytes (field 1)
    i64    cap;    // Capacity in bytes (field 2)
};
// LLVM representation: { ptr, i64, i64 }

createStringLiteral(str) {
    // 1. Create global constant for the string data
    strConst = ConstantDataArray::getString(ctx.llvmCtx, str);
    global = new llvm::GlobalVariable(*module, strConst->getType(),
                                      true, PrivateLinkage, strConst);

    // 2. Build the string struct { ptr, len, cap }
    ptr = builder.CreateBitCast(global, i8Ptr);
    len = ConstantInt::get(i64, str.length());

    result = UndefValue::get(strType);
    result = builder.CreateInsertValue(result, ptr, 0);
    result = builder.CreateInsertValue(result, len, 1);
    result = builder.CreateInsertValue(result, len, 2);
    return result;
}
```

#### 3.2.8 API Reference

| Method                                         | Description                                        |
| ---------------------------------------------- | -------------------------------------------------- |
| `storeValue(ValueDeclAST*, llvm::Value*)`      | Maps an AST declaration to an LLVM value           |
| `lookupValue(ValueDeclAST*)`                   | Retrieves the LLVM value for a declaration         |
| `storeFunction(FuncDeclAST*, llvm::Function*)` | Maps a function declaration to its LLVM function   |
| `lookupFunction(FuncDeclAST*)`                 | Retrieves the LLVM function for a declaration      |
| `getRuntimeFunction(const std::string&)`       | Retrieves a cached runtime function                |
| `getOrCreateRuntimeFunction(name, type)`       | Gets or creates a runtime function                 |
| `pushLoop(header, exit, continueTarget)`       | Pushes a loop context onto the stack               |
| `popLoop()`                                    | Pops the innermost loop context                    |
| `currentLoop()`                                | Returns the innermost loop context                 |
| `cacheType(TypeAST*, llvm::Type*)`             | Caches a type mapping                              |
| `lookupType(TypeAST*)`                         | Retrieves a cached type                            |
| `getStringType()`                              | Returns the string struct type `{ ptr, i64, i64 }` |
| `createStringLiteral(const std::string&)`      | Creates an LLVM string literal value               |
| `getLLVMIntrinsicDecl(id, argTypes)`           | Gets an LLVM intrinsic declaration                 |
| `parseOrdering(const std::string&)`            | Parses atomic memory ordering string               |
| `getTypeSize(llvm::Type*)`                     | Gets the size of a type in bytes                   |
| `getTypeAlign(llvm::Type*)`                    | Gets the alignment of a type in bytes              |

#### 3.2.9 Invariants

| Invariant             | Description                                                     |
| --------------------- | --------------------------------------------------------------- |
| **Module Non-Null**   | The `module` pointer must be non-null during code generation    |
| **Builder Valid**     | The `builder` must be valid and have a valid insertion point    |
| **Function Context**  | `currentFunction` must be set when lowering statements          |
| **Loop Context**      | `break`/`continue` statements must appear inside a loop context |
| **Symbol Uniqueness** | Each `ValueDeclAST*` maps to exactly one `llvm::Value*`         |
| **Type Uniqueness**   | Each `TypeAST*` maps to exactly one `llvm::Type*`               |
| **No Copy**           | Context is non-copyable                                         |

---

## 4. Module-Level Orchestration

The module-level orchestration is the entry point for code generation. It coordinates the two-phase lowering process and module verification.

```cpp
generate(modules, p, d, context)
│
├── for each module in modules:
│   ├── ctx = CodeGenContext(p, d, context)
│   ├── ctx.module = new llvm::Module(name, context)
│   ├── irModule = generateModule(module, ctx)
│   └── if irModule: result.push_back(move(irModule))
│
└── return result
```

```cpp
generateModule(module, ctx)
│
├── Phase 1: lowerModuleDeclarations(module, ctx)
│   ├── for each decl in module->decls:
│   │   ├── FuncDecl → lowerFunctionDecl() → Create prototype
│   │   ├── StructDecl → lowerStructDecl() → Create LLVM struct
│   │   ├── EnumDecl → lowerEnumDecl() → Create integer constants
│   │   └── VarDecl → lowerVarDecl() → Create global variable
│   └── Imports are handled by ModuleResolver, not CodeGen
│
├── Phase 2: lowerModuleBodies(module, ctx)
│   └── for each FuncDecl: lowerFunctionBody(decl, ctx)
│
└── Phase 3: Verify Module
    └── llvm::verifyModule(*ctx.module, &errorStream)
        ├── If verification fails: report diagnostic, return nullptr
        └── If verification passes: return unique_ptr(ctx.module)
```

**Key Points:**
- Each module gets its own `llvm::Module` and `CodeGenContext`
- Phase 1 registers all declarations so forward references work in Phase 2
- Module verification catches IR errors before returning the module

---

## 5. Type Mapping

Type mapping converts Lucid type annotations to LLVM types. Types are cached in `CodeGenContext` for performance.

### 5.1 Primitive Types

| Lucid Type                      | LLVM Type              |
| ------------------------------- | ---------------------- |
| `bool`                          | `i1`                   |
| `int8`, `uint8`, `byte`, `char` | `i8`                   |
| `int16`, `uint16`, `short`      | `i16`                  |
| `int32`, `uint32`, `int`        | `i32`                  |
| `int64`, `uint64`, `long`       | `i64`                  |
| `float`                         | `f32`                  |
| `double`                        | `f64`                  |
| `decimal`                       | `fp128`                |
| `string`                        | `ptr` (opaque pointer) |

### 5.2 Composite Types

| Lucid Type             | LLVM Type              |
| ---------------------- | ---------------------- |
| `[N]T` (fixed array)   | `[N x T]`              |
| `[*]T` (dynamic array) | `ptr` (opaque pointer) |
| `[_]T` (slice)         | `{ ptr, i64, i64 }`    |
| `T?` (nullable)        | `{ i8, T }`            |
| `T!` (fallible)        | `{ i8, T }`            |
| `T?!` (combined)       | `{ i8, T }`            |
| `&T` (reference)       | `ptr` (opaque pointer) |
| `*T` (raw pointer)     | `ptr` (opaque pointer) |
| `Future<T>`            | `{ T, i8 }`            |
| `Thread<T>`            | `{ T, i8 }`            |
| Function type          | `llvm::FunctionType`   |

### 5.3 Self-Referential Structs

Structs that reference themselves (e.g., linked lists) require special handling:

```cpp
lowerStructDecl(StructDeclAST* decl, ctx) {
    // 1. Create opaque struct FIRST (forward declaration)
    structType = llvm::StructType::create(ctx.llvmCtx, structName);

    // 2. Cache the opaque type BEFORE building fields
    ctx.cacheStruct(decl, structType);

    // 3. Build field types - getType() sees the self-reference
    //    and returns a pointer to the opaque type (breaks recursion)
    for (field : decl->fields) {
        fieldType = getType(ctx, field->type);  // Returns pointer to struct
        fieldTypes.push_back(fieldType);
    }

    // 4. Define the opaque struct with all field types
    structType->setBody(fieldTypes);
}
```

**Why this works:** When `getType()` encounters a `NamedTypeAST` that refers to the struct being defined, it finds the opaque type in the cache and returns a pointer to it. The pointer breaks the recursion.

---

## 6. Declaration Lowering

### 6.1 Function Declarations

Function declarations are lowered in two phases. Phase 1 creates the prototype, Phase 2 generates the body.

```cpp
Phase 1: lowerFunctionDecl() - Create prototype
│
├── 1. Check if already lowered → return
│
├── 2. Check for @[foreign] attribute
│   └── If foreign: create ExternalLinkage function (no body)
│
├── 3. Check for generic function
│   ├── If @[specialize]: register as template (no IR generated)
│   └── If default: generate type-erased version (tagged slots)
│
└── 4. Non-generic function:
    ├── funcType = getFunctionType(ctx, decl->funcType, hasClosure)
    ├── func = llvm::Function::Create(funcType, ExternalLinkage, name)
    ├── Set parameter names
    ├── ctx.storeFunction(decl, func)
    └── decl->llvmFunction = func
```

```cpp
Phase 2: lowerFunctionBody() - Generate body
│
├── 1. Skip foreign functions (no body)
├── 2. Skip @[specialize] generic functions (generated lazily)
├── 3. func = ctx.lookupFunction(decl)
└── 4. lowerFunctionBodyInternal(decl, func, ctx)
    ├── ctx.setCurrentFunction(func)
    ├── entryBlock = llvm::BasicBlock::Create("entry", func)
    ├── ctx.builder.SetInsertPoint(entryBlock)
    ├── For each parameter: lowerParam(param, ctx)
    │   ├── Create alloca for parameter
    │   ├── Store argument into alloca
    │   └── ctx.storeValue(param, alloca)
    ├── lowerStatement(decl->body, ctx)
    ├── ctx.setCurrentFunction(nullptr)
    └── llvm::verifyFunction(func)
```

### 6.2 Variable Declarations

Variables can be module-level (globals) or local (allocas).

```cpp
lowerVarDecl(VarDeclAST* decl, ctx) {
    varType = getType(ctx, decl->type);

    if (isModuleLevel) {
        // Global variable
        global = new llvm::GlobalVariable(*ctx.module, varType, isConst,
                                          ExternalLinkage, nullptr, name);
        if (decl->init) {
            initValue = lowerExpression(decl->init, ctx);
            if (llvm::Constant* constInit = dyn_cast<Constant>(initValue)) {
                global->setInitializer(constInit);
            }
        } else {
            global->setInitializer(Constant::getNullValue(varType));
        }
        ctx.storeValue(decl, global);
    } else {
        // Local variable
        alloca = createAlloca(name, varType, ctx);
        if (decl->init) {
            initValue = lowerExpression(decl->init, ctx);
            ctx.builder.CreateStore(initValue, alloca);
        } else {
            ctx.builder.CreateStore(Constant::getNullValue(varType), alloca);
        }
        ctx.storeValue(decl, alloca);
    }
}
```

### 6.3 Struct Declarations

Struct declarations are lowered to LLVM struct types.

```cpp
lowerStructDecl(StructDeclAST* decl, ctx) {
    if (ctx.lookupStruct(decl)) return;

    if (isGenericStruct(decl)) {
        if (shouldSpecialize(decl)) {
            // Register as template for lazy generation
            return;
        } else {
            // Generate type-erased version with tagged slots
            generateErasedGenericStruct(decl, ctx);
            return;
        }
    }

    // Non-generic struct
    // 1. Create opaque type
    structType = llvm::StructType::create(ctx.llvmCtx, structName);
    ctx.cacheStruct(decl, structType);

    // 2. Build field types (self-references resolve to pointers)
    for (field : decl->fields) {
        fieldType = getType(ctx, field->type);
        fieldTypes.push_back(fieldType);
    }

    // 3. Define the struct
    structType->setBody(fieldTypes);
    decl->llvmType = structType;
}
```

### 6.4 Enum Declarations

Enums are lowered to integer constants with a backing type.

```cpp
lowerEnumDecl(EnumDeclAST* decl, ctx) {
    backingType = getEnumType(ctx, decl);  // Default: i32

    for (variant : decl->variants) {
        constVal = llvm::ConstantInt::get(backingType, variant->value);
        variant->llvmValue = constVal;

        // Create global constant for the variant
        new llvm::GlobalVariable(*ctx.module, backingType, true,
                                 ExternalLinkage, constVal, varName);
    }
    decl->backingLLVMType = backingType;
}
```

---

## 7. Statement Lowering

### 7.1 Control Flow Statements

```cpp
If Statement
│
├── cond = lowerExpression(stmt->condition)
├── thenBlock = BasicBlock::Create("if_then", func)
├── elseBlock = BasicBlock::Create("if_else", func)  // or mergeBlock
├── mergeBlock = BasicBlock::Create("if_merge", func)
│
├── builder.CreateCondBr(cond, thenBlock, elseBlock)
│
├── builder.SetInsertPoint(thenBlock)
├── lowerStatement(stmt->thenBranch)
├── if (!block->getTerminator()) builder.CreateBr(mergeBlock)
│
├── if (stmt->elseBranch) {
│   builder.SetInsertPoint(elseBlock)
│   lowerStatement(stmt->elseBranch)
│   if (!block->getTerminator()) builder.CreateBr(mergeBlock)
│ }
│
└── builder.SetInsertPoint(mergeBlock)
```

```cpp
Switch Statement
│
├── subject = lowerExpression(stmt->subject)
│
├── // Build case blocks
├── for (case : stmt->cases) {
│   caseBlock = BasicBlock::Create("case", func)
│   for (value : case->values) {
│       if (value is LiteralExpr) {
│           caseValues.push_back(lowerLiteralExpr(value))
│           caseBodyBlocks.push_back(caseBlock)
│       }
│   }
│ }
│
├── // Create switch instruction
├── switchInst = builder.CreateSwitch(subject, defaultBlock, caseCount)
├── for (i : caseValues) {
│   switchInst->addCase(caseValues[i], caseBodyBlocks[i])
│ }
│
├── // Lower case bodies
├── for (case : stmt->cases) {
│   builder.SetInsertPoint(caseBlock)
│   lowerStatement(case->body)
│   if (!block->getTerminator()) builder.CreateBr(mergeBlock)
│ }
│
└── builder.SetInsertPoint(mergeBlock)
```

### 7.2 Loops

```cpp
For Loop (Range-based)
│
├── // Lower range bounds
├── startVal = lowerExpression(range->lo)
├── endVal = lowerExpression(range->hi)
│
├── // Allocate and initialize loop variable
├── alloca = createAlloca(indexVar->name, idxType, ctx)
├── builder.CreateStore(startVal, alloca)
│
├── // Header block
├── builder.SetInsertPoint(headerBlock)
├── current = builder.CreateLoad(idxType, alloca)
├── cond = builder.CreateICmpSLT(current, endVal)
├── builder.CreateCondBr(cond, bodyBlock, exitBlock)
│
├── // Body block
├── builder.SetInsertPoint(bodyBlock)
├── lowerStatement(stmt->body)
├── builder.CreateBr(continueBlock)
│
├── // Continue block
├── builder.SetInsertPoint(continueBlock)
├── incremented = builder.CreateAdd(current, stepVal)
├── builder.CreateStore(incremented, alloca)
└── builder.CreateBr(headerBlock)
```

```cpp
For Loop (Collection-based)
│
├── // Get array data and length
├── collection = lowerExpression(stmt->iterable)
├── len = getArrayLength(collection, arrayType, ctx)
├── dataPtr = getDataPointer(collection, arrayType, ctx)
│
├── // Allocate index variable
├── idxAlloca = createAlloca("_loop_idx", int64Ty, ctx)
├── builder.CreateStore(ConstantInt::get(int64Ty, 0), idxAlloca)
│
├── // Header block
├── builder.SetInsertPoint(headerBlock)
├── currentIdx = builder.CreateLoad(int64Ty, idxAlloca)
├── cond = builder.CreateICmpSLT(currentIdx, len)
├── builder.CreateCondBr(cond, bodyBlock, exitBlock)
│
├── // Body block - load element
├── builder.SetInsertPoint(bodyBlock)
├── elemPtr = builder.CreateGEP(elemType, dataPtr, currentIdx)
├── elemVal = builder.CreateLoad(elemType, elemPtr)
├── if (valueVar) builder.CreateStore(elemVal, valueAlloca)
├── lowerStatement(stmt->body)
├── builder.CreateBr(continueBlock)
│
├── // Continue block
├── builder.SetInsertPoint(continueBlock)
├── nextIdx = builder.CreateAdd(currentIdx, 1)
├── builder.CreateStore(nextIdx, idxAlloca)
└── builder.CreateBr(headerBlock)
```

```cpp
While Loop
│
├── builder.CreateBr(headerBlock)
│
├── builder.SetInsertPoint(headerBlock)
├── cond = lowerExpression(stmt->condition)
├── builder.CreateCondBr(cond, bodyBlock, exitBlock)
│
├── builder.SetInsertPoint(bodyBlock)
├── lowerStatement(stmt->body)
└── builder.CreateBr(headerBlock)
```

### 7.3 Concurrency Statements

```cpp
Async Statement
│
├── asyncFunc = ctx.getRuntimeFunction("__lucid_async")
├── callResult = lowerExpression(stmt->call)
│
├── if (stmt->binding) {
│   bindingValue = createAlloca(binding->name, bindingType, ctx)
│   builder.CreateStore(callResult, bindingValue)
│   ctx.storeValue(stmt->binding, bindingValue)
│ }
│
└── builder.CreateCall(asyncFunc, {callResult, null, null})
```

```cpp
Await Statement
│
├── awaitFunc = ctx.getRuntimeFunction("__lucid_await")
│
└── for (target : stmt->targets) {
    bindingValue = ctx.lookupValue(target->resolvedDecl)
    builder.CreateCall(awaitFunc, {bindingValue})
}
```

```cpp
Spawn Statement
│
├── spawnFunc = ctx.getRuntimeFunction("__lucid_spawn")
├── callResult = lowerExpression(stmt->call)
│
├── if (stmt->binding) {
│   bindingValue = createAlloca(binding->name, bindingType, ctx)
│   builder.CreateStore(callResult, bindingValue)
│   ctx.storeValue(stmt->binding, bindingValue)
│ }
│
└── builder.CreateCall(spawnFunc, {callResult, null})
```

```cpp
Join Statement
│
├── joinFunc = ctx.getRuntimeFunction("__lucid_join")
│
└── for (target : stmt->targets) {
    bindingValue = ctx.lookupValue(target->resolvedDecl)
    builder.CreateCall(joinFunc, {bindingValue})
}
```

---

## 8. Expression Lowering

### 8.1 Literals and Identifiers

```cpp
Literals
├── True/False     → ConstantInt::get(type, 1/0)
├── Int/Hex/Binary → ConstantInt::get(type, stoll(value))
├── Float          → ConstantFP::get(type, stod(value))
├── String         → GlobalVariable with ConstantDataArray
├── Char           → ConstantInt::get(type, value[0])
└── Nil/Err        → Constant::getNullValue(type)
```

```cpp
Array Literals
│
├── // Evaluate all elements
├── for (elem : expr->elements) {
│   elemValue = lowerExpression(elem, ctx)
│   elements.push_back(elemValue)
│ }
│
├── if (arrayType->isFixed()) {
│   // Fixed array: create constant array
│   return ConstantArray::get(arrayType, elements)
│ } else {
│   // Dynamic array: allocate and store elements
│ }
```

```cpp
Identifiers
│
├── // Look up from symbol table
├── value = ctx.lookupValue(expr->resolvedDecl)
├── if (!value) {
│   value = ctx.lookupFunction(expr->resolvedDecl)
│ }
│
├── // Load if l-value
├── if (expr->isLValue) {
│   return builder.CreateLoad(type, value)
│ }
│
└── return value
```

### 8.2 Binary Operators

```cpp
lowerBinaryExpr(expr, ctx) {
    left = lowerExpression(expr->left, ctx)
    right = lowerExpression(expr->right, ctx)

    if (expr->left->isLValue) {
        left = loadIfNeeded(left, getType(left->getType()), ctx)
    }
    if (expr->right->isLValue) {
        right = loadIfNeeded(right, getType(right->getType()), ctx)
    }

    switch (expr->op) {
        case Add:  if (integer) return CreateAdd(left, right)
                   else return CreateFAdd(left, right)
        case Sub:  if (integer) return CreateSub(left, right)
                   else return CreateFSub(left, right)
        case Mul:  if (integer) return CreateMul(left, right)
                   else return CreateFMul(left, right)
        case Div:  if (integer) {
                       checkedRight = emitZeroCheck(right, DivisionByZero)
                       return CreateSDiv(left, checkedRight)
                   } else return CreateFDiv(left, right)
        case Eq:   if (integer) return CreateICmpEQ(left, right)
                   else return CreateFCmpUEQ(left, right)
        case And:  if (!isBool(left)) left = CreateICmpNE(left, 0)
                   if (!isBool(right)) right = CreateICmpNE(right, 0)
                   return CreateAnd(left, right)
        // ... other operators ...
    }
}
```

### 8.3 Calls and Intrinsics

```cpp
Call Expression
│
├── calleeVal = lowerExpression(expr->callee, ctx)
├── callee = dyn_cast<llvm::Function>(calleeVal)
│
├── for (arg : expr->args) {
│   argVal = lowerExpression(arg, ctx)
│   if (arg->isLValue) {
│       argVal = loadIfNeeded(argVal, getType(arg->resolvedType), ctx)
│   }
│   args.push_back(argVal)
│ }
│
└── return builder.CreateCall(callee, args, "call")
```

```cpp
Intrinsic Call Expression
│
└── // Dispatch to intrinsic emitter
    return emitIntrinsicFromAST(expr, ctx)

// Example: #sqrt(x)
// → call @llvm.sqrt.f32(float %x)
```

### 8.4 Arrays and Slices

```cpp
Index Expression
│
├── target = lowerExpression(expr->target, ctx)
├── index = lowerExpression(expr->index, ctx)
│
├── // Bounds check
├── len = getArrayLength(target, arrayType, ctx)
├── checkedIndex = emitBoundsCheck(index, len, ctx)
│
├── // GEP
├── ptr = getDataPointer(target, arrayType, ctx)
├── gep = builder.CreateGEP(elemType, ptr, checkedIndex)
│
├── if (expr->isLValue) {
│   return gep  // Return pointer for assignment
│ }
│
└── return builder.CreateLoad(elemType, gep)
```

```cpp
Slice Expression
│
├── // Evaluate bounds (default to 0 and len)
├── start = expr->start ? lowerExpression(expr->start) : 0
├── end = expr->end ? lowerExpression(expr->end) : len
│
├── // Bounds check
├── (checkedStart, checkedEnd) = emitSliceBoundsCheck(start, end, len)
│
├── // Offset data pointer
├── dataPtr = getDataPointer(target, arrayType, ctx)
├── slicePtr = builder.CreateGEP(elemType, dataPtr, checkedStart)
│
├── // Calculate length and capacity
├── sliceLen = builder.CreateSub(checkedEnd, checkedStart)
├── sliceCap = builder.CreateSub(len, checkedStart)
│
├── // Build slice struct { ptr, len, cap }
├── slice = UndefValue::get(sliceType)
├── slice = builder.CreateInsertValue(slice, slicePtr, 0)
├── slice = builder.CreateInsertValue(slice, sliceLen, 1)
├── slice = builder.CreateInsertValue(slice, sliceCap, 2)
│
└── return slice
```

### 8.5 Pipeline and Composition

```cpp
Pipeline Expression
│
├── // Lower seed
├── currentValue = lowerExpression(expr->seed)
├── if (expr->seed->isLValue) {
│   currentValue = loadIfNeeded(currentValue, ...)
│ }
│
├── // Apply each step
├── for (step : expr->steps) {
│   currentValue = lowerPipelineStep(step, ctx)
│ }
│
└── return currentValue
```

```cpp
Pipeline Step
│
├── // Lower callable and arguments
├── callable = lowerExpression(step->callable)
├── for (arg : step->packArgs) {
│   argVal = lowerExpression(arg)
│   args.push_back(argVal)
│ }
│
├── // Cast to function type
├── fnType = FunctionType::get(returnType, paramTypes, false)
├── typedFunc = builder.CreatePointerCast(callable, PointerType(fnType))
│
└── // Call
    return builder.CreateCall(fnType, typedFunc, args, "pipeline_call")
```

---

## 9. Generic Instantiation

Generic instantiation is the process of creating concrete versions of generic functions and structs with specific type arguments. Lucid supports two strategies for generics: **monomorphization** (via `@[specialize]`) and **type erasure** (default).

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Generic Instantiation Overview                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Generic Declaration:                                                       │
│  const identity<T> (x T) -> T = { return x }                                │
│  struct Box<T> { value T }                                                  │
│                                                                             │
│                              ┌─────────────────────┐                        │
│                              │   @[specialize]?    │                        │
│                              └──────────┬──────────┘                        │
│                                         │                                   │
│                    ┌────────────────────┴────────────────────┐              │
│                    │                                         │              │
│                    ▼                                         ▼              │
│  ┌────────────────────────────────┐  ┌────────────────────────────────────┐ │
│  │  Type Erasure (Default)        │  │  Monomorphization (@[specialize])  │ │
│  ├────────────────────────────────┤  ├────────────────────────────────────┤ │
│  │  • One function per generic    │  │  • One function per instantiation  │ │
│  │  • Tagged slots { tag, value } │  │  • No runtime overhead             │ │
│  │  • Runtime tag checking        │  │  • Direct calls                    │ │
│  │  • Generated immediately       │  │  • Generated lazily on first use   │ │
│  │  • Smaller binary size         │  │  • Larger binary size              │ │
│  └────────────────────────────────┘  └────────────────────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 9.1 Detection Helpers

The detection helpers determine whether a declaration is generic and how it should be handled.

```cpp
isGenericFunction(FuncDeclAST* decl)
└── return decl && !decl->genericParams.empty()

isGenericStruct(StructDeclAST* decl)
└── return decl && !decl->genericParams.empty()

shouldSpecialize(DeclAST* decl)
├── if (decl->isa<FuncDeclAST>())
│   └── return decl->as<FuncDeclAST>()->shouldSpecialize
├── if (decl->isa<StructDeclAST>())
│   └── return decl->as<StructDeclAST>()->shouldSpecialize
└── return false
```

### 9.2 Generic Substitution Context

The `GenericSubstitution` struct maps generic parameter names to concrete type arguments.

```cpp
struct GenericSubstitution {
    const ArenaSpan<GenericParamDeclAST*>& genericParams;
    const std::vector<TypeAST*>& typeArgs;

    // Find the type argument for a generic parameter name
    TypeAST* lookup(InternedString name) const {
        for (size_t i = 0; i < genericParams.size(); ++i) {
            if (genericParams[i]->name == name && i < typeArgs.size()) {
                return typeArgs[i];
            }
        }
        return nullptr;
    }

    // Check if a name is a generic parameter
    bool isGenericParam(InternedString name) const {
        for (const auto* param : genericParams) {
            if (param->name == name) return true;
        }
        return false;
    }
};
```

### 9.3 Specialized Instantiation Creation

```cpp
createSpecializedFunction()
│
├── 1. Generate mangled name: generateMangledNameForGeneric()
│
├── 2. Build parameter types with substitution
│   ├── GenericSubstitution subst{funcDecl->genericParams, typeArgs}
│   └── For each parameter: getType(ctx, param->type, &subst)
│
├── 3. Build return type with substitution
│   └── getType(ctx, funcDecl->funcType->returnType, &subst)
│
├── 4. Create llvm::Function with mangled name
│   ├── Check if already exists
│   └── llvm::Function::Create(llvmFuncType, InternalLinkage, name)
│
├── 5. Set parameter names
│
└── 6. Return llvm::Function*
```

```cpp
createSpecializedStruct()
│
├── 1. Generate mangled name: generateMangledNameForGeneric()
│
├── 2. Build field types with substitution
│   ├── GenericSubstitution subst{structDecl->genericParams, typeArgs}
│   └── For each field: getType(ctx, field->type, &subst)
│
├── 3. Check if struct already exists
│   └── llvm::StructType::getTypeByName(ctx.llvmCtx, structName)
│
├── 4. If exists and is opaque: setBody() (complete forward declaration)
│
├── 5. If not exists: create new struct type
│
└── 6. Return llvm::Type*
```

### 9.4 Type-Erased Generic Generation

```cpp
TaggedSlot Type:
struct TaggedSlot {
    i8   tag;    // 0 = valid, 1 = nil, 2 = err
    void* value; // Opaque pointer to the actual value
};
// LLVM representation: { i8, ptr }
```

```cpp
generateErasedGenericFunction()
│
├── 1. Generate mangled name (module-qualified + "__erased")
│   └── moduleName + "_" + funcName + "__erased"
│
├── 2. Build parameter types (all opaque pointers)
│
├── 3. Return type is opaque pointer
│
├── 4. Create llvm::Function with mangled name
│
└── 5. Return llvm::Function*
```

```cpp
generateErasedGenericStruct()
│
├── 1. Get or create the canonical TaggedSlot type
│
├── 2. Generate mangled name (module-qualified + "__erased")
│
├── 3. Build field types (all TaggedSlot)
│
├── 4. Create llvm::StructType with mangled name
│
└── 5. Return llvm::Type*
```

### 9.5 Public Registry API

```cpp
getOrCreateSpecializedFunction()
│
├── if (!funcDecl || !isGenericFunction(funcDecl)) return nullptr
│
├── // Type-erased generics
├── if (!shouldSpecialize(funcDecl)) {
│   return generateErasedGenericFunction(funcDecl, ctx)
│ }
│
├── // Monomorphized generics
├── GenericInstantiationKey key{funcDecl, typeArgs}
│
├── // Check cache
├── if (cached) return cached
│
├── // Create new specialization
├── specialized = createSpecializedFunction(funcDecl, typeArgs, ctx)
│
├── // Cache it
├── ctx.genericRegistry.functionInstantiations[funcDecl][key] = specialized
│
└── return specialized
```

### 9.6 Mangled Name Generation

```cpp
Mangled Name Format:
_L<module>_<name>_G<args>_P<params>_R<return>

Type Encoding:
┌─────────────────────────────────────────────────────────────────────────────┐
│  Primitive Types:                                                           │
│  bool → b  int8 → c  int16 → s  int32 → i  int64 → l                        │
│  uint8 → h  uint16 → t  uint32 → u  uint64 → m                              │
│  float → f  double → d  decimal → D  string → S  char → C                   │
├─────────────────────────────────────────────────────────────────────────────┤
│  Composite Types:                                                           │
│  Array:    A<size><elem>     [10]int → A10i                                 │
│  Slice:    A_<elem>          [_]int → A_i                                   │
│  Pointer:  P<inner>          *int → Pi                                      │
│  Reference: R<inner>         &int → Ri                                      │
│  Nullable: N<inner>          int? → Ni                                      │
│  Fallible: F<inner>          int! → Fi                                      │
│  Function: F<params>_<ret>   (int)->int → Fi_i                              │
│  Named:    <name>_G<args>    Box<int> → Box_Gi                              │
└─────────────────────────────────────────────────────────────────────────────┘

Examples:
┌─────────────────────────────────────────────────────────────────────────────┐
│  identity<int> → _Lmain_identity_Gi_Pi_Ri                                   │
│  Box<int>      → _Lmain_Box_Gi_Fi                                           │
│  pair<int,float> → _Lmain_pair_Gi_f_Pi_f_R*                                 │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 9.7 Comparison of Generic Strategies

| Aspect                    | Type Erasure (Default)           | Monomorphization (@[specialize])       |
| ------------------------- | -------------------------------- | -------------------------------------- |
| **Binary Size**           | Small (one function per generic) | Large (one function per instantiation) |
| **Runtime Overhead**      | Tag checking on each operation   | Zero overhead                          |
| **Compile Time**          | Fast (no instantiation)          | Slower (multiple instantiations)       |
| **Optimization**          | Limited (type-agnostic)          | Full (type-specialized)                |
| **Forward Compatibility** | Works with unknown types         | Requires all types known               |
| **Use Case**              | Library code, rarely used        | Hot paths, performance-critical        |

---

## 10. Support Helpers

The CodeGen phase relies on a set of helper utilities that provide low-level LLVM IR construction primitives, type introspection, memory allocation, and runtime panic handling.

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Support Helpers Overview                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────────────┐  │
│  │  CodeGenAlloca  │  │  CodeGenHelpers │  │  CodeGenPanic               │  │
│  ├─────────────────┤  ├─────────────────┤  ├─────────────────────────────┤  │
│  │ • createAlloca  │  │ • getArrayLength│  │ • emitPanic                 │  │
│  │ • createBlock   │  │ • lowerRangeFor │  │ • emitNullCheck             │  │
│  │ • loadIfNeeded  │  │   Loop          │  │ • emitBoundsCheck           │  │
│  └─────────────────┘  │ • lowerCollect │  │ • emitSliceBoundsCheck       │  │
│                       │   ForLoop       │  │ • emitZeroCheck             │  │
│                       └─────────────────┘  └─────────────────────────────┘  │
│                                                                             │
│  ┌─────────────────┐  ┌──────────────────────────────────────────────────┐  │
│  │  LLVMHelpers    │  │  RuntimeError                                    │  │
│  ├─────────────────┤  ├──────────────────────────────────────────────────┤  │
│  │ • Type          │  │ • RuntimeErrorKind enum                          │  │
│  │   Predicates    │  │ • getRuntimeErrorMessage()                       │  │
│  │ • Value         │  │ • toDiagCode()                                   │  │
│  │   Predicates    │  └──────────────────────────────────────────────────┘  │
│  │ • Type          │                                                        │
│  │   Extraction    │                                                        │
│  │ • Type          │                                                        │
│  │   Constants     │                                                        │
│  └─────────────────┘                                                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 10.1 Memory Allocation and Basic Blocks (`CodeGenAlloca`)

```cpp
createAlloca(name, type, ctx)
│
├── func = ctx.getCurrentFunction()
├── if (!func) return nullptr
│
├── // Always insert at the beginning of the entry block
├── entryBlock = &func->getEntryBlock()
├── builder.SetInsertPoint(entryBlock, entryBlock->getFirstInsertionPt())
│
└── return builder.CreateAlloca(type, nullptr, name)
```

```cpp
createBlock(name, ctx)
│
├── func = ctx.getCurrentFunction()
├── if (!func) return nullptr
│
└── return llvm::BasicBlock::Create(ctx.llvmCtx, name, func)
```

```cpp
loadIfNeeded(value, elemType, ctx)
│
├── if (!value || !elemType) return value
│
├── if (value->getType()->isPointerTy()) {
│   return ctx.builder.CreateLoad(elemType, value)
│ }
│
└── return value
```

### 10.2 General CodeGen Helpers (`CodeGenHelpers`)

```cpp
getArrayLength(target, arrayType, ctx)
│
├── i64Ty = llvm::Type::getInt64Ty(ctx.llvmCtx)
│
├── switch (arrayType->arrayKind) {
│   case Fixed:
│       return llvm::ConstantInt::get(i64Ty, arrayType->size)
│
│   case Dynamic:
│       // Length stored before data: [len: i64][data: T*]
│       lenPtr = builder.CreatePtrToInt(target, i64Ty)
│       lenPtr = builder.CreateSub(lenPtr, ConstantInt::get(i64Ty, 8))
│       lenPtr = builder.CreateIntToPtr(lenPtr, ptrType)
│       return builder.CreateLoad(i64Ty, lenPtr)
│
│   case Slice:
│       // Slice: { ptr, len, cap }
│       return builder.CreateExtractValue(target, 1)
│ }
```

### 10.3 Runtime Panic and Safety Checks (`CodeGenPanic`)

```cpp
emitPanic(kind, ctx)
│
├── // Format: "[E4101] division by zero"
├── message = "[" + formatDiagCodePrefix(toDiagCode(kind)) + "] "
│           + getRuntimeErrorMessage(kind)
│
├── panicFunc = getOrCreateRuntimeFunction("__lucid_panic", panicType)
│
├── // Create string constant for message
├── msgGlobal = createGlobalString(message)
│
└── builder.CreateCall(panicFunc, {msgPtr})
    builder.CreateUnreachable()
```

```cpp
emitNullCheck(ptr, ctx)
│
├── if (!ptr || !ptr->getType()->isPointerTy()) return ptr
│
├── func = ctx.getCurrentFunction()
│
├── isNull = builder.CreateICmpEQ(ptr, Constant::getNullValue(ptrType))
│
├── passBlock = BasicBlock::Create("null_check_pass", func)
├── failBlock = BasicBlock::Create("null_check_fail", func)
├── mergeBlock = BasicBlock::Create("null_check_merge", func)
│
├── builder.CreateCondBr(isNull, failBlock, passBlock)
│
├── builder.SetInsertPoint(passBlock)
├── builder.CreateBr(mergeBlock)
│
├── builder.SetInsertPoint(failBlock)
├── emitPanic(NullPointerDereference, ctx)
├── builder.CreateBr(mergeBlock)
│
├── builder.SetInsertPoint(mergeBlock)
├── phi = builder.CreatePHI(ptrType, 2)
├── phi->addIncoming(ptr, passBlock)
├── phi->addIncoming(Constant::getNullValue(ptrType), failBlock)
│
└── return phi
```

```cpp
emitBoundsCheck(index, length, ctx)
│
├── // Cast to i64
├── index = builder.CreateIntCast(index, i64Ty, true)
├── length = builder.CreateIntCast(length, i64Ty, true)
│
├── outOfBounds = builder.CreateOr(
│   builder.CreateICmpSLT(index, 0),
│   builder.CreateICmpSGE(index, length)
│ )
│
├── passBlock = BasicBlock::Create("bounds_ok", func)
├── failBlock = BasicBlock::Create("bounds_fail", func)
├── mergeBlock = BasicBlock::Create("bounds_merge", func)
│
├── builder.CreateCondBr(outOfBounds, failBlock, passBlock)
│
├── builder.SetInsertPoint(passBlock)
├── builder.CreateBr(mergeBlock)
│
├── builder.SetInsertPoint(failBlock)
├── emitPanic(ArrayIndexOutOfBounds, ctx)
├── builder.CreateBr(mergeBlock)
│
├── builder.SetInsertPoint(mergeBlock)
├── phi = builder.CreatePHI(i64Ty, 2)
├── phi->addIncoming(index, passBlock)
├── phi->addIncoming(ConstantInt::get(i64Ty, 0), failBlock)
│
└── return phi
```

### 10.4 LLVM Type Helpers (`LLVMHelpers`)

```cpp
Type Predicates:
├── isIntegerType(type)    → type && type->isIntegerTy()
├── isFloatType(type)      → type && type->isFloatingPointTy()
├── isPointerType(type)    → type && type->isPointerTy()
├── isStructType(type)     → type && type->isStructTy()
├── isArrayType(type)      → type && type->isArrayTy()
├── isVoidType(type)       → type && type->isVoidTy()
├── isNumericType(type)    → isIntegerType(type) || isFloatType(type)
└── isFunctionType(type)   → type && type->isFunctionTy()

Composite Type Predicates:
├── isTaggedType(type)     → struct { i8 tag, T value }
├── isSliceType(type)      → struct { ptr, i64 len, i64 cap }
├── isClosureType(type)    → struct { ptr func, ptr env }
└── isStringType(type)     → struct { ptr data, i64 len, i64 cap }

Type Constants:
├── getI8Type(ctx)         → llvm::Type::getInt8Ty(ctx)
├── getI32Type(ctx)        → llvm::Type::getInt32Ty(ctx)
├── getI64Type(ctx)        → llvm::Type::getInt64Ty(ctx)
├── getI1Type(ctx)         → llvm::Type::getInt1Ty(ctx)
├── getFloatType(ctx)      → llvm::Type::getFloatTy(ctx)
├── getDoubleType(ctx)     → llvm::Type::getDoubleTy(ctx)
├── getVoidType(ctx)       → llvm::Type::getVoidTy(ctx)
├── getPtrType(ctx)        → llvm::PointerType::get(ctx, 0)
└── getStringType(ctx)     → llvm::StructType::get(ctx, {i8Ptr, i64, i64})
```

### 10.5 Runtime Error Registry (`RuntimeError`)

```cpp
RuntimeErrorKind enum:
├── // Arithmetic Errors
│   DivisionByZero, ModuloByZero, IntegerOverflow, NegationOverflow
├── // Array/Slice Errors
│   ArrayIndexOutOfBounds, SliceBoundsOutOfRange, NegativeArraySize
├── // Pointer Errors
│   NullPointerDereference, DanglingPointer
├── // Memory Errors
│   DoubleFree, FreeNullPointer, AllocationFailed
├── // Type System Errors
│   UnwrappedNil, UnwrappedErr, TagMismatch
└── // Concurrency Errors
    AwaitOnNonFuture, JoinOnNonThread, FutureAlreadyConsumed, ThreadAlreadyJoined
.
getRuntimeErrorMessage(kind)
└── returns the error message string for the given kind
.
toDiagCode(kind)
└── maps RuntimeErrorKind to compile-time DiagCode
    └── Same error at runtime and compile-time shows the SAME diagnostic code
```

### 10.6 Helper Usage Across CodeGen

```cpp
CodeGenDecl.cpp
├── #include "support/CodeGenAlloca.hpp"
├── #include "support/CodeGenPanic.hpp"
└── Used for: createAlloca(), loadIfNeeded()

CodeGenStmt.cpp
├── #include "support/CodeGenAlloca.hpp"
├── #include "support/CodeGenHelpers.hpp"
└── Used for: createBlock(), lowerRangeForLoop(), lowerCollectionForLoop()

CodeGenExpr.cpp
├── #include "support/CodeGenPanic.hpp"
├── #include "support/LLVMHelpers.hpp"
└── Used for: emitBoundsCheck(), emitZeroCheck(), isIntegerType()

CodeGenType.cpp
├── #include "support/LLVMHelpers.hpp"
└── Used for: getI8Type(), getI64Type(), isStructType()
```

---

## 11. Closure Lowering

Closure lowering is the process of translating anonymous functions that capture variables from their enclosing scope into LLVM IR.

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Closure Lowering Overview                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Three-Component Architecture:                                              │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  ENVIRONMENT STRUCT                                                   │  │
│  │  struct closure_env_1 {                                               │  │
│  │      int count;                                                       │  │
│  │  };                                                                   │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                    │                                        │
│                                    ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  CLOSURE FUNCTION                                                     │  │
│  │  define internal int @closure_1(ptr %env, int %step) {                │  │
│  │      %count = load int, ptr %env                                      │  │
│  │      %result = add %count, %step                                      │  │
│  │      store %result, ptr %env                                          │  │
│  │      ret %result                                                      │  │
│  │  }                                                                    │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                    │                                        │
│                                    ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  FAT POINTER (Closure Value)                                          │  │
│  │  struct closure {                                                     │  │
│  │      void* func;  // points to @closure_1                             │  │
│  │      void* env;   // points to environment                            │  │
│  │  };                                                                   │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 11.1 Capture Handling

```cpp
Capture Types:
├── BY VALUE (byReference = false)
│   ├── The value is COPIED into the environment
│   └── Example: const f () -> int = { return x }  // x is copied
│
└── BY REFERENCE (byReference = true)
    ├── A POINTER to the variable is stored in the environment
    ├── The closure can MUTATE the original variable
    └── Example: let count int = 0; const f () -> int = { count += 1 }
```

```cpp
Capture Rules:
├── Rule 1: No Borrowed Types
│   └── Closures cannot capture &T or [_]T (enforced by Sema)
│
├── Rule 2: No Linear Types
│   └── Closures cannot capture Future<T> or Thread<T>
│
└── Rule 3: Escaping Closures are Heap-Allocated
    ├── Non-escaping: stack-allocated
    └── Escaping: heap-allocated (returned from function)
```

### 11.2 Closure Lowering Steps

```cpp
lowerClosure(expr, ctx)
│
├── Step 1: Validate Closure State
│   └── if (no captures) return plain function pointer
│
├── Step 2: Build Environment Struct
│   └── envType = buildClosureEnvironment(expr, ctx)
│
├── Step 3: Create Closure Function
│   └── closureFunc = createClosureFunction(expr, ctx)
│
├── Step 4: Store Function and Environment on AST
│   ├── expr->closureFunction = closureFunc
│   └── expr->environmentType = envType
│
├── Step 5: Get Runtime Alloc Function
│   └── allocEnv = getOrCreateRuntimeFunction("__lucid_alloc_env")
│
├── Step 6: Calculate Environment Size
│   └── envSize = DataLayout.getTypeAllocSize(envType)
│
├── Step 7: Allocate Environment on Heap
│   └── envPtr = builder.CreateCall(allocEnv, {envSizeVal})
│
├── Step 8: Fill Environment with Captured Values
│   └── for (capture : expr->captures) {
│         fieldPtr = builder.CreateStructGEP(envType, envPtr, index)
│         builder.CreateStore(capturedValue, fieldPtr)
│       }
│
├── Step 9: Handle By-Value vs By-Reference
│   ├── if (!byReference) capturedValue = loadIfNeeded(...)
│   └── else capturedValue is already a pointer
│
├── Step 10: Create Closure Value (Fat Pointer)
│   └── closure = { closureFunc, envPtr }
│
└── Step 11: Return Closure Value
    └── expr->llvmValue = closure
```

### 11.3 Environment Struct Building

```cpp
buildClosureEnvironment(expr, ctx)
│
├── // 1. If no captures, return empty struct
├── if (expr->captures.empty()) {
│   return llvm::StructType::create(ctx.llvmCtx, "closure_env_empty")
│ }
│
├── // 2. Build field types for each captured variable
├── for (capture : expr->captures) {
│   fieldType = getCaptureFieldType(ctx, capture)
│   fieldTypes.push_back(fieldType)
│ }
│
├── // 3. Create the environment struct
├── envName = "closure_env_" + counter
├── envType = llvm::StructType::create(ctx.llvmCtx, fieldTypes, envName)
│
└── expr->environmentType = envType
    return envType
```

### 11.4 Closure Function Creation

```cpp
createClosureFunction(expr, ctx)
│
├── 1. Get the function type from the AST
│   └── funcType = expr->funcType
│
├── 2. Build parameter types
│   ├── Environment pointer (first param)
│   └── Regular parameters (after env)
│
├── 3. Build return type
│   └── getType(ctx, funcType->returnType)
│
├── 4. Create the LLVM function
│   ├── funcName = "closure_" + counter
│   └── llvm::Function::Create(fnType, InternalLinkage, name)
│
├── 5. Set parameter names
│   ├── arg 0: "env"
│   └── args 1..N: parameter names
│
└── 6. Emit the function body
    └── emitClosureBody(expr, closureFunc, envPtr, ctx)
```

### 11.5 Closure Call Emission

```cpp
emitClosureCall(funcPtr, envPtr, args, ctx)
│
├── 1. Build function type from arguments
│   └── paramTypes: [env_ptr, arg1_type, arg2_type, ...]
│
├── 2. Cast function pointer to correct type
│   └── typedFunc = CreatePointerCast(funcPtr, PointerType(fnType))
│
├── 3. Build argument list
│   └── callArgs = [envPtr, arg1, arg2, ...]
│
└── 4. Create the call
    └── result = builder.CreateCall(fnType, typedFunc, callArgs)
```

---

## 12. Intrinsic Emission

Intrinsic emission is the process of translating Lucid intrinsic calls (`#name(...)`) into LLVM IR.

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Intrinsic Emission Overview                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  LLVM Intrinsics          → Direct LLVM intrinsic calls               │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  • Math: sqrt, abs, fma, ceil, floor, round, pow, min, max            │  │
│  │  • Memory: memcpy, memmove, memset                                    │  │
│  │  • Bit ops: clz, ctz, popcount, bswap                                 │  │
│  │  • Atomics: atomic_load, atomic_store, atomic_add, etc.               │  │
│  │  • SIMD: simd_add, simd_load, simd_splat, etc.                        │  │
│  │  • CPU hints: prefetch, fence, pause                                  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  Lucid Intrinsics        → Compiler-implemented operations            │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  • Type: sizeof, alignof, typeof, nameof, tostr, ptrstr               │  │
│  │  • Pointers: toRef, toPtr, ptrOffset, ptrDiff, addrof                 │  │
│  │  • Memory: alloc, free, arena_create, arena_alloc, etc.               │  │
│  │  • String: str_len, str_ptr, str_concat, str_slice, etc.              │  │
│  │  • Control: likely, unlikely, scope_exit                              │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 12.1 Intrinsic Dispatcher

```cpp
emitIntrinsicFromAST(expr, ctx)
│
├── // Special-case intrinsics that need raw addresses
├── if (name == "addrof") {
│   return lowerExpression(expr->args[0], ctx)  // Do NOT load
│ }
├── if (name == "toRef") {
│   return emitNullCheck(lowerExpression(expr->args[0]), ctx)  // Do NOT load
│ }
│
├── // Normal path: load l-values
├── for (arg : expr->args) {
│   argVal = lowerExpression(arg, ctx)
│   if (arg->isLValue) {
│       elemType = getType(ctx, arg->resolvedType)
│       argVal = loadIfNeeded(argVal, elemType, ctx)
│   }
│   args.push_back(argVal)
│ }
│
├── // Dispatch by category
├── if (isLLVMIntrinsic(name)) {
│   return emitLLVMIntrinsic(name, args, expr, ctx)
│ } else if (isLucidIntrinsic(name)) {
│   return emitLucidIntrinsic(name, args, expr, ctx)
│ }
│
└── error("unknown intrinsic")
```

### 12.2 LLVM Math Intrinsics

```cpp
emitLLVMMathIntrinsic(name, args, expr, ctx)
│
├── // min / max
├── if (name == "min" || name == "max") {
│   pred = (name == "min") ? ICMP_SLT : ICMP_SGT
│   cmp = builder.CreateICmp(pred, a, b)
│   return builder.CreateSelect(cmp, a, b)
│ }
│
├── // pow
├── if (name == "pow") {
│   if (a->isIntegerTy()) a = builder.CreateSIToFP(a, doubleTy)
│   func = getOrInsertFunction("pow", powType)
│   return builder.CreateCall(func, {a, b})
│ }
│
├── // abs
├── if (name == "abs") {
│   if (a->isIntegerTy()) {
│       absFn = getLLVMIntrinsicDecl(Intrinsic::abs, {a->getType()})
│       return builder.CreateCall(absFn, {a, false})
│   }
│   if (a->isFloatingPointTy()) {
│       fabsFn = getLLVMIntrinsicDecl(Intrinsic::fabs, {a->getType()})
│       return builder.CreateCall(fabsFn, {a})
│   }
│ }
│
└── // sqrt / fma / ceil / floor / round
    id = nameToIntrinsicID(name)
    intrinsic = getLLVMIntrinsicDecl(id, {args[0]->getType()})
    return builder.CreateCall(intrinsic, args)
```

**Lucid → LLVM Intrinsic Mapping:**

| Lucid Intrinsic | LLVM Intrinsic                                        |
| --------------- | ----------------------------------------------------- |
| `#sqrt(x)`      | `@llvm.sqrt.f32(float %x)`                            |
| `#fma(a,b,c)`   | `@llvm.fma.f32(float %a, float %b, float %c)`         |
| `#ceil(x)`      | `@llvm.ceil.f32(float %x)`                            |
| `#floor(x)`     | `@llvm.floor.f32(float %x)`                           |
| `#round(x)`     | `@llvm.round.f32(float %x)`                           |
| `#abs(x)`       | `@llvm.abs.i32(i32 %x)` or `@llvm.fabs.f32(float %x)` |

### 12.3 LLVM Memory Intrinsics

```cpp
emitLLVMMemoryIntrinsic(name, args, expr, ctx)
│
├── // Map Lucid name to LLVM intrinsic ID
├── if (name == "memcpy")   id = Intrinsic::memcpy
├── if (name == "memmove")  id = Intrinsic::memmove
├── if (name == "memset")   id = Intrinsic::memset
│
├── // Get LLVM intrinsic declaration
├── intrinsic = getLLVMIntrinsicDecl(id, args->getTypes())
│
├── // Add optional isVolatile parameter
├── if (args.size() < 4) {
│   args.push_back(ConstantInt::get(i1, 0))  // not volatile
│ }
│
└── return builder.CreateCall(intrinsic, args)
```

### 12.4 LLVM Bit Manipulation Intrinsics

```cpp
emitLLVMBitIntrinsic(name, args, expr, ctx)
│
├── // Map to LLVM intrinsic ID
├── if (name == "clz")       id = Intrinsic::ctlz
├── if (name == "ctz")       id = Intrinsic::cttz
├── if (name == "popcount")  id = Intrinsic::ctpop
├── if (name == "bswap")     id = Intrinsic::bswap
│
├── // Get declaration
├── intrinsic = getLLVMIntrinsicDecl(id, {args[0]->getType()})
│
├── // clz/ctz need is_zero_poison parameter
├── if (name == "clz" || name == "ctz") {
│   args.push_back(ConstantInt::get(i1, 0))  // don't poison on zero
│ }
│
└── return builder.CreateCall(intrinsic, args)
```

### 12.5 LLVM Atomic Intrinsics

```cpp
emitLLVMAtomicIntrinsic(name, args, expr, ctx)
│
├── // Parse ordering (last argument, if string literal)
├── if (tryGetStringLiteralArg(expr, args.size() - 1, orderStr)) {
│   ordering = parseOrdering(orderStr)
│   numValueArgs--
│ }
│
├── // atomic_load(ptr)
├── if (name == "atomic_load") {
│   elemType = getType(ctx, expr->resolvedType)
│   load = builder.CreateLoad(elemType, ptr)
│   load->setAtomic(ordering)
│   return load
│ }
│
├── // atomic_store(ptr, val)
├── if (name == "atomic_store") {
│   store = builder.CreateStore(val, ptr)
│   store->setAtomic(ordering)
│   return nullptr
│ }
│
├── // atomic_add/sub/and/or/xor
├── if (name matches "atomic_*") {
│   op = nameToRMWOp(name)
│   return builder.CreateAtomicRMW(op, ptr, val, align, ordering)
│ }
│
└── // atomic_cas(ptr, expected, desired)
    if (name == "atomic_cas") {
        cas = builder.CreateAtomicCmpXchg(ptr, expected, desired, ordering)
        return builder.CreateExtractValue(cas, 1)  // success flag
    }
```

**Memory Ordering Mapping:**

| Lucid Ordering | LLVM AtomicOrdering                      |
| -------------- | ---------------------------------------- |
| `"relaxed"`    | `AtomicOrdering::Monotonic`              |
| `"acquire"`    | `AtomicOrdering::Acquire`                |
| `"release"`    | `AtomicOrdering::Release`                |
| `"acq_rel"`    | `AtomicOrdering::AcquireRelease`         |
| `"seq_cst"`    | `AtomicOrdering::SequentiallyConsistent` |

### 12.6 LLVM SIMD Intrinsics

```cpp
SIMD Arithmetic:
├── simd_add → isFloat ? FAdd : Add
├── simd_sub → isFloat ? FSub : Sub
├── simd_mul → isFloat ? FMul : Mul
└── simd_div → isFloat ? FDiv : SDiv

SIMD FMA:
└── simd_fma → getLLVMIntrinsicDecl(Intrinsic::fma, {a->getType(), b->getType(), c->getType()})

SIMD Min/Max:
└── simd_min/simd_max → pred = (name == "simd_min") ? ICMP_SLT : ICMP_SGT

SIMD Splat:
└── simd_splat → builder.CreateVectorSplat(vecType->getElementCount(), scalar)

SIMD Extract:
└── simd_extract → builder.CreateExtractElement(vec, index)  // index must be const

SIMD Insert:
└── simd_insert → builder.CreateInsertElement(vec, val, index)  // index must be const

SIMD Load/Store:
├── simd_load  → builder.CreateLoad(vecType, ptr)
└── simd_store → builder.CreateStore(val, ptr)
```

### 12.7 LLVM CPU Hint Intrinsics

```cpp
prefetch / prefetch_r / prefetch_w
│
├── rw = (name == "prefetch_w") ? 1 : 0
├── locality = 3
├── cacheType = 0
│
├── prefetch = getLLVMIntrinsicDecl(Intrinsic::prefetch, {ptr->getType()})
│
└── return builder.CreateCall(prefetch, {ptr, rw, locality, cacheType})

fence
└── builder.CreateFence(ordering)

pause
└── builder.CreateFence(SequentiallyConsistent)
```

### 12.8 Lucid Type Inspection Intrinsics

```cpp
sizeof(T)
└── return ConstantInt::get(i64, ctx.getTypeSize(llvmType))

alignof(T)
└── return ConstantInt::get(i64, ctx.getTypeAlign(llvmType))

bitcast(T, x)
└── return builder.CreateBitCast(args[0], targetType)

typeof(x)
└── return ctx.createStringLiteral(getLucidTypeName(ctx, type))

nameof(x)
└── return ctx.createStringLiteral(nameStr)
```

### 12.9 Lucid Pointer Intrinsics

```cpp
toPtr(ref) → return args[0]

ptrOffset(ptr, n)
├── elemType = recoverPointeeType(expr, ctx)
└── return builder.CreateInBoundsGEP(elemType, ptr, offset)

ptrDiff(p1, p2)
├── diffBytes = builder.CreateSub(builder.CreatePtrToInt(p1, i64),
│                                 builder.CreatePtrToInt(p2, i64))
├── elemSize = recoverElementSize(expr, ctx)
├── if (elemSize > 1) {
│   return builder.CreateSDiv(diffBytes, elemSize)
│ }
└── return diffBytes
```

### 12.10 Lucid Memory Management Intrinsics

```cpp
alloc(T, count)
├── elemSize = recoverElementSize(expr, ctx)
├── size = builder.CreateMul(count, ConstantInt::get(i64, elemSize))
├── allocFunc = getOrCreateRuntimeFunction("__lucid_alloc", allocType)
├── result = builder.CreateCall(allocFunc, {size})
└── return builder.CreateBitCast(result, targetType)

free(ptr)
├── freeFunc = getOrCreateRuntimeFunction("__lucid_free", freeType)
└── builder.CreateCall(freeFunc, {builder.CreateBitCast(ptr, i8Ptr)})

arena_create(size)
└── return getOrCreateRuntimeFunction("__lucid_arena_create")({size})

arena_alloc(arena, T, n)
├── elemSize = recoverElementSize(expr, ctx)
├── size = builder.CreateMul(args[1], ConstantInt::get(i64, elemSize))
├── arenaAllocFunc = getOrCreateRuntimeFunction("__lucid_arena_alloc")
├── result = builder.CreateCall(arenaAllocFunc, {args[0], size})
└── return builder.CreateBitCast(result, targetType)

arena_reset(arena)
└── getOrCreateRuntimeFunction("__lucid_arena_reset")({args[0]})

arena_free(arena)
└── getOrCreateRuntimeFunction("__lucid_arena_free")({args[0]})
```

### 12.11 Lucid String Intrinsics

```cpp
str_len(s)      → builder.CreateExtractValue(args[0], 1)
str_ptr(s)      → builder.CreateExtractValue(args[0], 0)

str_from_ptr(ptr, len)
├── str = UndefValue::get(strType)
├── str = builder.CreateInsertValue(str, ptr, 0)
├── str = builder.CreateInsertValue(str, len, 1)
├── str = builder.CreateInsertValue(str, len, 2)
└── return str

str_concat(a, b)
└── getOrCreateRuntimeFunction("__lucid_str_concat")({args[0], args[1]})

str_slice(s, from, to)
└── getOrCreateRuntimeFunction("__lucid_str_slice")({args[0], args[1], args[2]})

str_eq(a, b)
└── getOrCreateRuntimeFunction("__lucid_str_eq")({args[0], args[1]})

str_byte_at(s, i)
├── ptr = builder.CreateExtractValue(args[0], 0)
├── bytePtr = builder.CreateGEP(i8, ptr, args[1])
└── return builder.CreateLoad(i8, bytePtr)
```

### 12.12 Lucid Control Flow Intrinsics

```cpp
scope_exit
└── // Handled in Sema and stored on BlockStmtAST
    // CodeGenStmt.cpp emits these callbacks
    // No runtime code generated at the call site

likely / unlikely
└── return args[0]  // Condition value - branch weight metadata added later
```