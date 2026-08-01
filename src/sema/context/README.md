# Context Stack Documentation

## Overview

The Context Stack is the core tracking mechanism during semantic analysis. It maintains the state needed to answer questions like "Are we inside a function?" or "What type was this variable narrowed to?"

### Quick Reference

| Question                                | Method                          | Section                                         |
| --------------------------------------- | ------------------------------- | ----------------------------------------------- |
| Are we inside a function body?          | `insideFunction()`              | [Context Tracking](#1-context-tracking)         |
| Are we inside a loop?                   | `insideLoop()`                  | [Context Tracking](#1-context-tracking)         |
| Are we inside an if condition?          | `isIfConditionCtx()`            | [Type Narrowing](#2-type-narrowing)             |
| What's the narrowed type of variable X? | `getNarrowedType(X)`            | [Type Narrowing](#2-type-narrowing)             |
| Does the current function need returns? | `hasReturnRequirements()`       | [Return Requirements](#3-return-requirements)   |
| Are all returns satisfied?              | `returnRequirementsSatisfied()` | [Return Requirements](#3-return-requirements)   |
| Is a type currently being defined?      | `isDefiningType(T)`             | [Self-Reference](#4-self-reference-detection)   |
| Look up a value by name                 | `lookupValue(name)`             | [Symbol Storage](#5-symbol-storage)             |
| Look up a type by name                  | `lookupType(name)`              | [Symbol Storage](#5-symbol-storage)             |
| Insert a value declaration              | `insertValue(decl)`             | [Symbol Storage](#5-symbol-storage)             |
| Is there a pending async operation?     | `hasPendingAsync(name)`         | [Concurrency Tracking](#6-concurrency-tracking) |
| Is there a pending spawn operation?     | `hasPendingSpawn(name)`         | [Concurrency Tracking](#6-concurrency-tracking) |

### Features

The Context Stack manages six main concerns:

1. **Context Tracking** - What semantic construct are we currently inside? (function, loop, if, block, etc.)
2. **Type Narrowing** - Refining variable types based on conditional checks
3. **Return Requirements** - Tracking return obligations in curried functions
4. **Self-Reference Detection** - Detecting when a type references itself during definition
5. **Symbol Storage** - Two-tier symbol table for declarations
6. **Concurrency Tracking** - Tracking pending async/spawn operations at scope level

---

## 1. Context Tracking

### Overview

Context tracking maintains a stack of semantic contexts. Each frame represents 
a construct like a function body, loop, if statement, or block. This answers 
questions about what statements are legal (e.g., `return` is only legal inside a function body).

### Context Kinds

| Kind         | Description               | Legal Statements                   |
| ------------ | ------------------------- | ---------------------------------- |
| `TopLevel`   | Module-level declarations | Declarations only                  |
| `FuncBody`   | Inside a function body    | `return` allowed                   |
| `LoopBody`   | Inside a loop             | `break`, `continue` allowed        |
| `SwitchBody` | Inside a switch           | `case`, `default` allowed          |
| `IfStmt`     | Inside an if statement    | Type narrowing applied             |
| `Block`      | Inside a block            | Pending inverse narrowing tracking |

### Context Stack Structure

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        Context Stack                                    │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Top of Stack (innermost)                                         │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Frame: Block                                               │  │  │
│  │  │  node: thenBranch                                           │  │  │
│  │  │  kind: Block                                                │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Frame: IfStmt                                              │  │  │
│  │  │  node: if statement                                         │  │  │
│  │  │  kind: IfStmt                                               │  │  │
│  │  │  isIfConditionCtx: false                                    │  │  │
│  │  │  hasElse: true                                              │  │  │
│  │  │  pendingNarrowing: {x → int}                                │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Frame: FuncBody                                            │  │  │
│  │  │  node: process function                                     │  │  │
│  │  │  kind: FuncBody                                             │  │  │
│  │  │  returnReqs: { groups: [{returnType: int, requires: true}] }│  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Frame: TopLevel                                            │  │  │
│  │  │  node: module                                               │  │  │
│  │  │  kind: TopLevel                                             │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  Bottom of Stack (outermost)                                            │
└─────────────────────────────────────────────────────────────────────────┘
```

### Push/Pop Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        Push/Pop Flow                                    │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │  Entering function                                                 │ │
│  │  ┌───────────────────────────────────────────────────────────────┐ │ │
│  │  │  ctx.stack.pushFunction(func)                                 │ │ │
│  │  │  └── m_stack.push_back(frame with FuncBody)                   │ │ │
│  │  └───────────────────────────────────────────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                    │                                    │
│                                    ▼                                    │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │  Entering if statement                                             │ │
│  │  ┌───────────────────────────────────────────────────────────────┐ │ │
│  │  │  ctx.stack.push(ContextKind::IfStmt, stmt)                    │ │ │
│  │  │  └── m_stack.push_back(frame with IfStmt)                     │ │ │
│  │  └───────────────────────────────────────────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                    │                                    │
│                                    ▼                                    │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │  Entering loop                                                     │ │
│  │  ┌───────────────────────────────────────────────────────────────┐ │ │
│  │  │  ctx.stack.pushLoop(loopStmt)                                 │ │ │
│  │  │  └── m_stack.push_back(frame with LoopBody)                   │ │ │
│  │  └───────────────────────────────────────────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                    │                                    │
│                                    ▼                                    │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │  Exiting contexts (reverse order)                                  │ │
│  │  ┌───────────────────────────────────────────────────────────────┐ │ │
│  │  │  ctx.stack.pop()  // LoopBody                                 │ │ │
│  │  │  ctx.stack.pop()  // IfStmt                                   │ │ │
│  │  │  ctx.stack.pop()  // FuncBody                                 │ │ │
│  │  └───────────────────────────────────────────────────────────────┘ │ │
│  └────────────────────────────────────────────────────────────────────┘ │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### Pseudocode

```python
# Entering a function
def resolveFuncDecl(func, ctx):
    ctx.stack.pushFunction(func, func.funcType, func.loc)
    # Analyze body...
    ctx.stack.pop()

# Entering an if statement
def resolveIfStmt(stmt, ctx):
    ctx.stack.push(ContextKind.IfStmt, stmt, stmt.loc)
    # Analyze condition and branches...
    ctx.stack.pop()

# Querying context
def resolveReturnStmt(stmt, ctx):
    if not ctx.stack.insideFunction():
        error("return outside function")
        return True
    # ... validate return ...
```

---

## 2. Type Narrowing

### Overview

Type narrowing refines variable types based on conditional checks. 
When the compiler sees `if x != nil`, it knows `x` is non-nullable inside the then branch. 
This is a core feature for safe nullable/fallible handling.

### Narrowing Stack

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     Type Narrowing Stack                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Innermost Level (current)                                        │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Level 2 - Then Branch                                      │  │  │
│  │  │  isInverse: false                                           │  │  │
│  │  │  narrowedTypes: { "x" → int, "y" → string }                 │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Level 1 - Block Level                                      │  │  │
│  │  │  isInverse: true                                            │  │  │
│  │  │  narrowedTypes: { "x" → int }                               │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Level 0 - Function/Module Level                            │  │  │
│  │  │  isInverse: false                                           │  │  │
│  │  │  narrowedTypes: {}                                          │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  Search Order: Innermost → Outermost                                    │
└─────────────────────────────────────────────────────────────────────────┘
```

### Narrowing Types

| Type                          | Description                                       | Example                                             |
| ----------------------------- | ------------------------------------------------- | --------------------------------------------------- |
| **Direct Narrowing**          | Condition directly proves the variable has a type | `if x != nil { ... }` → x is int                    |
| **Inverse Narrowing**         | Condition proves the opposite would have exited   | `if x == nil { return }` → x is int                 |
| **Pending Inverse Narrowing** | Deferred inverse narrowing for standalone if      | `if x == nil { return }` → applied to rest of block |

### Narrowing Rules Table

| Condition          | Inside Block      | Rest of Scope (Inverse)   |
| ------------------ | ----------------- | ------------------------- |
| `a == nil`         | a is nil          | a is non-nullable         |
| `a != nil`         | a is non-nullable | a is nullable (no change) |
| `a == err`         | a is err          | a is non-fallible         |
| `a != err`         | a is non-fallible | a is fallible (no change) |
| `or` at top level  | N/A               | All variables narrowed    |
| `and` at top level | N/A               | No narrowing (unsound)    |

### Narrowing Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                      Type Narrowing Flow                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │  Step 1: Analyze condition                                             │ │
│  │                                                                        │ │
│  │  ctx.stack.setIfConditionCtx(true)                                     │ │
│  │  resolveExpr(condition)                                                │ │
│  │      └── In resolveBinaryExpr: detectNarrowingPattern()                │ │
│  │          └── Returns NarrowingInfo {x → int, isEquality: false}        │ │
│  │  ctx.stack.setPendingNarrowing(info)                                   │ │
│  │  ctx.stack.setIfConditionCtx(false)                                    │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                      │                                      │
│                                      ▼                                      │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │  Step 2: Then branch (direct narrowing)                                │ │
│  │                                                                        │ │
│  │  if info.hasNarrowing and not info.isEquality:                         │ │
│  │      ctx.stack.pushNarrowingLevel(false)                               │ │
│  │      for each (name, type) in info.narrowings:                         │ │
│  │          ctx.stack.narrowVariable(name, type)                          │ │
│  │      resolveBlock(thenBranch)                                          │ │
│  │      ctx.stack.popNarrowingLevel()                                     │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                      │                                      │
│                                      ▼                                      │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │  Step 3: Else branch (inverse narrowing)                               │ │
│  │                                                                        │ │
│  │  if info.hasNarrowing and info.isEquality:                             │ │
│  │      ctx.stack.pushNarrowingLevel(true)                                │ │
│  │      for each (name, type) in info.narrowings:                         │ │
│  │          ctx.stack.narrowVariable(name, type)                          │ │
│  │      resolveBlock(elseBranch)                                          │ │
│  │      ctx.stack.popNarrowingLevel()                                     │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                      │                                      │
│                                      ▼                                      │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │  Step 4: Standalone if with early exit                                 │ │
│  │                                                                        │ │
│  │  if !hasElse && thenReturns && hasNarrowing && info.isEquality:        │ │
│  │      ctx.stack.setPendingInverseNarrowing(info)                        │ │
│  │      └── Applied when resolveBlock(parentBlock) continues              │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## # Pseudocode

```python
def resolveIfStmt(stmt, ctx):
    # 1. Push if context
    ctx.stack.push(ContextKind.IfStmt, stmt, stmt.loc)
    ctx.stack.setHasElse(stmt.elseBranch is not None)
    
    # 2. Analyze condition with narrowing detection
    ctx.stack.setIfConditionCtx(True)
    resolveExpr(stmt.condition)
    ctx.stack.setIfConditionCtx(False)
    
    # 3. Get narrowing info
    info = ctx.stack.getPendingNarrowing()
    ctx.stack.clearPendingNarrowing()
    
    # 4. Then branch with direct narrowing
    if info.hasNarrowing and not info.isEquality:
        ctx.stack.pushNarrowingLevel(False)
        for (name, type) in info.narrowings:
            ctx.stack.narrowVariable(name, type)
        thenReturns = resolveBlock(stmt.thenBranch)
        ctx.stack.popNarrowingLevel()
    
    # 5. Else branch with inverse narrowing
    if hasElse:
        if info.hasNarrowing and info.isEquality:
            ctx.stack.pushNarrowingLevel(True)
            for (name, type) in info.narrowings:
                ctx.stack.narrowVariable(name, type)
            elseReturns = resolveBlock(stmt.elseBranch)
            ctx.stack.popNarrowingLevel()
    
    # 6. Standalone if with early exit
    if not hasElse and thenReturns and info.isEquality:
        ctx.stack.setPendingInverseNarrowing(info)
    
    ctx.stack.pop()
```

---

## 3. Return Requirements

### Overview

Return requirements track return obligations in curried functions. Each `->` in a function signature creates a "return group" that must be satisfied by a `return` statement at the correct nesting level.

### Core Concept

A curried function like `(a int) -> (int) -> int` has two return groups:

```lucid
const add (a int) -> (int) -> int = {
    // Group 0: (a int) -> (int) -> int
    //   → requires returning a function
    // Group 1: (b int) -> int
    //   → requires returning an int
    
    return (b int) -> int {
        return a + b
    }
}
```

### Return Requirements Structure

```
┌───────────────────────────────────────────────────────────────────────────────┐
│                      Return Requirements Structure                            │
├───────────────────────────────────────────────────────────────────────────────┤
│                                                                               │
│  ReturnRequirements {                                                         │
│      groups: [                                                                │
│          Group {                                                              │
│              returnType: FuncTypeAST,    // (b int) -> int                    │
│              requiresReturn: true,                                            │
│              isSatisfied: false,                                              │
│              level: 0,                     // Nesting level                   │
│              isCurried: true                                                  │
│          },                                                                   │
│          Group {                                                              │
│              returnType: PrimitiveTypeAST,  // int                            │
│              requiresReturn: true,                                            │
│              isSatisfied: false,                                              │
│              level: 1,                     // Nesting level                   │
│              isCurried: false                                                 │
│          }                                                                    │
│      ],                                                                       │
│      currentGroupIndex: -1,                 // Next group to satisfy          │
│      currentLevel: 0,                       // Current nesting depth          │
│      isVoid: false,                                                           │
│      allowsOptionalReturn: false                                              │
│  }                                                                            │
│                                                                               │
└───────────────────────────────────────────────────────────────────────────────┘
```

### Pseudocode

```python
def buildReturnRequirements(funcType):
    reqs = ReturnRequirements()
    current = funcType
    level = 0
    
    while current:
        group = ReturnRequirements.Group(
            returnType=current.returnType,
            requiresReturn=current.hasArrow,
            isCurried=current.returnType is FuncTypeAST,
            level=level if group.requiresReturn else level,
            isSatisfied=False
        )
        if group.requiresReturn:
            level += 1
        reqs.groups.append(group)
        
        if group.isCurried:
            current = current.returnType
        else:
            break
    
    reqs.isVoid = not lastGroup.requiresReturn or lastGroup.returnType is None
    return reqs

def resolveReturnStmt(stmt, ctx):
    if not ctx.stack.insideFunction():
        error("return outside function")
        return True
    
    currentGroup = ctx.stack.currentReturnGroup()
    
    if stmt.value:
        if not currentGroup or not currentGroup.requiresReturn:
            error("return value not expected")
            return True
        checkType(stmt.value, currentGroup.returnType)
        ctx.stack.advanceReturnGroup()
    else:
        if currentGroup and currentGroup.requiresReturn:
            error("void return but function expects value")
            return True
    
    return True
```

---

## 4. Self-Reference Detection

### Overview

Self-reference detection tracks types currently being defined to distinguish between legal and illegal self-references. A struct field `next Node<T>` would create infinite size (illegal), but `next *Node<T>?` (pointer + nullable) is legal.

### Self-Reference Stack

```
┌─────────────────────────────────────────────────────────────────────────┐
│                     Self-Reference Stack                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  Currently Defining Types                                         │  │
│  │                                                                   │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Top: Graph<T> (being defined)                              │  │  │
│  │  │  └── Field: edge *Edge? → self-ref via ptr?                 │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Middle: Node<T> (being defined)                            │  │  │
│  │  │  └── Field: next *Node<T>? → self-ref via ptr ✅            │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  │  ┌─────────────────────────────────────────────────────────────┐  │  │
│  │  │  Bottom: Entity (being defined)                             │  │  │
│  │  │  └── Field: parent Entity? → self-ref via nullable ✅       │  │  │
│  │  └─────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│                                                                         │
│  Self-Reference Rules:                                                  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │  T      → ❌ ERROR (infinite size)                                │  │
│  │  T?     → ✅ OK (nullable)                                        │  │
│  │  *T     → ✅ OK (pointer breaks the cycle)                        │  │
│  │  *T?    → ✅ OK (nullable pointer)                                │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

### Pseudocode

```python
def resolveNamedType(namedType, ctx):
    # 1. Check if generic parameter
    if ctx.isGenericParam(namedType.name):
        return namedType
    
    # 2. Look up type
    decl = ctx.lookupType(namedType.name)
    if not decl:
        error("undefined type")
        return None
    
    # 3. Check self-reference
    if ctx.isDefiningType(decl):
        # Self-reference detected - check wrapping
        pass
    
    return namedType

def checkSelfReference(fieldType, currentStruct, ctx):
    result = SelfReferenceInfo()
    result.isSelfReference = False
    
    # Unwrap nullable
    isNullable = False
    if fieldType is NullableTypeAST:
        isNullable = True
        innerType = fieldType.inner
    
    # Unwrap pointer
    isPointer = False
    if innerType is PtrTypeAST:
        isPointer = True
        innerType = innerType.inner
    
    # Check if named type matches current struct
    if innerType is NamedTypeAST:
        if innerType.name == currentStruct.name:
            if sameGenericInstantiation(innerType, currentStruct):
                result.isSelfReference = True
                result.isPointer = isPointer
                result.isNullable = isNullable
                result.isNonNullable = not isPointer and not isNullable
    
    return result
```

---

## 5. Symbol Storage

### Overview

Symbol storage is a two-tier symbol table that tracks declarations during semantic analysis. It maintains persistent module-level tables and transient scopes for local variables.

### Two-Tier Structure

```
┌────────────────────────────────────────────────────────────────────────────┐
│                      Two-Tier Symbol Storage                               │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  ┌────────────────────────────────────────────────────────────────────────┐│
│  │  Persistent ModuleTable (per module, never erased)                     ││
│  │  ┌───────────────────────────────────────────────────────────────────┐ ││
│  │  │  ModuleTable {                                                    │ ││
│  │  │      module: ModuleAST*                                           │ ││
│  │  │      values: { "add" → FuncDecl, "PI" → VarDecl, ... }            │ ││
│  │  │      types:  { "Vec2" → Struct, "Direction" → Enum, ... }         │ ││
│  │  │      importAliases: { "math" → ModuleAST, ... }                   │ ││
│  │  │  }                                                                │ ││
│  │  └───────────────────────────────────────────────────────────────────┘ ││
│  └────────────────────────────────────────────────────────────────────────┘│
│                                                                            │
│  ┌────────────────────────────────────────────────────────────────────────┐│
│  │  Transient Scopes (pushed/popped per block, function, etc.)            ││
│  │                                                                        ││
│  │  ┌───────────────────────────────────────────────────────────────────┐ ││
│  │  │  Scope (innermost)                                                │ ││
│  │  │  values: { "x" → VarDecl, "temp" → Param }                        │ ││
│  │  │  types: {}                                                        │ ││
│  │  │  genericParams: { "T" → GenericParam }                            │ ││
│  │  │  pendingAsync: { "result" → PendingAsync }                        │ ││
│  │  │  pendingSpawn: { "heavy" → PendingSpawn }                         │ ││
│  │  └───────────────────────────────────────────────────────────────────┘ ││
│  │  ┌───────────────────────────────────────────────────────────────────┐ ││
│  │  │  Scope (middle)                                                   │ ││
│  │  │  values: { "y" → VarDecl }                                        │ ││
│  │  │  types: {}                                                        │ ││
│  │  │  genericParams: {}                                                │ ││
│  │  └───────────────────────────────────────────────────────────────────┘ ││
│  │  ┌───────────────────────────────────────────────────────────────────┐ ││
│  │  │  Scope (outermost)                                                │ ││
│  │  │  values: { "counter" → VarDecl }                                  │ ││
│  │  │  types: {}                                                        │ ││
│  │  │  genericParams: {}                                                │ ││
│  │  └───────────────────────────────────────────────────────────────────┘ ││
│  └────────────────────────────────────────────────────────────────────────┘│
│                                                                            │
│  Lookup Priority: 1. Generic params → 2. Scopes → 3. ModuleTable           │
└────────────────────────────────────────────────────────────────────────────┘
```

### Pseudocode

```python
def lookupValue(name, ctx):
    # 1. Search scopes from innermost to outermost
    for scope in reversed(ctx.scopes):
        if name in scope.values:
            return scope.values[name]
    
    # 2. Fall back to module table
    if name in ctx.currentModuleTable.values:
        return ctx.currentModuleTable.values[name]
    
    # 3. Not found
    return None

def lookupType(name, ctx):
    # 1. Search scopes
    for scope in reversed(ctx.scopes):
        # Generic parameters shadow type names
        if name in scope.genericParams:
            return None
        if name in scope.types:
            return scope.types[name]
    
    # 2. Fall back to module table
    if name in ctx.currentModuleTable.types:
        return ctx.currentModuleTable.types[name]
    
    return None

def insertValue(decl, ctx):
    if ctx.isAtModuleLevel():
        ctx.currentModuleTable.values[decl.name] = decl
    else:
        ctx.currentScope().values[decl.name] = decl
```

---

## 6. Concurrency Tracking

### Overview

Concurrency tracking manages pending `async` and `spawn` operations at the scope level. `async` and `spawn` are statements that schedule operations, not contexts that enclose blocks. The tracking is done by collecting unresolved async/spawn bindings that must be resolved before the scope exits.

### Core Concept

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    Concurrency Tracking                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │  Scope Level Tracking                                                   ││
│  │                                                                         ││
│  │  ┌───────────────────────────────────────────────────────────────────┐  ││
│  │  │  Scope {                                                          │  ││
│  │  │      pendingAsync: { "result1", "result2", "userData" }           │  ││
│  │  │      pendingSpawn: { "heavyResult", "processedData" }             │  ││
│  │  │  }                                                                │  ││
│  │  └───────────────────────────────────────────────────────────────────┘  ││
│  │                                                                         ││
│  │  When a scope exits, check if all operations were resolved:             ││
│  │  ┌───────────────────────────────────────────────────────────────────┐  ││
│  │  │  if unresolved:                                                   │  ││
│  │  │      warning("operation was never awaited/joined")                │  ││
│  │  └───────────────────────────────────────────────────────────────────┘  ││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │  Async/Spawn Resolution                                                 ││
│  │                                                                         ││
│  │  ┌───────────────────────────────────────────────────────────────────┐  ││
│  │  │  async result = fetchData(url)                                    │  ││
│  │  │  └── pendingAsync["result"] = fetchData(url)                      │  ││
│  │  │                                                                   │  ││
│  │  │  await result                                                     │  ││
│  │  │  └── pendingAsync.erase("result")  // ✅ Resolved                 │  ││
│  │  └───────────────────────────────────────────────────────────────────┘  ││
│  │                                                                         ││
│  │  ┌───────────────────────────────────────────────────────────────────┐  ││
│  │  │  spawn heavy = computeHeavy()                                     │  ││
│  │  │  └── pendingSpawn["heavy"] = computeHeavy()                       │  ││
│  │  │                                                                   │  ││
│  │  │  join heavy                                                       │  ││
│  │  │  └── pendingSpawn.erase("heavy")  // ✅ Resolved                  │  ││
│  │  └───────────────────────────────────────────────────────────────────┘  ││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Async/Spawn Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    Async/Spawn Statement Flow                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │  Step 1: async/spawn statement                                          ││
│  │                                                                         ││
│  │  async result = fetchData(url)                                          ││
│  │  spawn heavyResult = computeHeavy()                                     ││
│  │                                                                         ││
│  │  ┌─────────────────────────────────────────────────────────────────────┐││
│  │  │  if target is not '_':                                              │││
│  │  │      // Store in pending list for current scope                     │││
│  │  │      if async:                                                      │││
│  │  │          ctx.addPendingAsync(target, call, loc)                     │││
│  │  │      else:                                                          │││
│  │  │          ctx.addPendingSpawn(target, call, loc)                     │││
│  │  │  else:                                                              │││
│  │  │      // '_' discard - no tracking needed                            │││
│  │  └─────────────────────────────────────────────────────────────────────┘││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                      │                                      │
│                                      ▼                                      │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │  Step 2: await/join statement                                           ││
│  │                                                                         ││
│  │  await result1, result2                                                 ││
│  │  join heavyResult                                                       ││
│  │                                                                         ││
│  │  ┌─────────────────────────────────────────────────────────────────────┐││
│  │  │  for each name in targets:                                          │││
│  │  │      if ctx.hasPendingAsync(name):                                  │││
│  │  │          ctx.resolveAsync(name)  // Remove from pending             │││
│  │  │      elif ctx.hasPendingSpawn(name):                                │││
│  │  │          ctx.resolveSpawn(name)  // Remove from pending             │││
│  │  │      else:                                                          │││
│  │  │          error("no pending operation for ", name)                   │││
│  │  └─────────────────────────────────────────────────────────────────────┘││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                      │                                      │
│                                      ▼                                      │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │  Step 3: Scope exit (in resolveBlock)                                   ││
│  │                                                                         ││
│  │  ┌─────────────────────────────────────────────────────────────────────┐││
│  │  │  // Check for unresolved operations                                 │││
│  │  │  for name in ctx.getPendingAsyncNames():                            │││
│  │  │      warning("async '", name, "' was never awaited")                │││
│  │  │                                                                     │││
│  │  │  for name in ctx.getPendingSpawnNames():                            │││
│  │  │      warning("spawn '", name, "' was never joined")                 │││
│  │  └─────────────────────────────────────────────────────────────────────┘││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Pseudocode

```python
def resolveAsyncStmt(stmt, ctx):
    if not ctx.stack.insideFunction():
        error("async outside function body")
        return False
    
    targetName = stmt.target.name
    if targetName != "_":
        ctx.addPendingAsync(targetName, stmt.call, stmt.loc)
    
    return False

def resolveAwaitStmt(stmt, ctx):
    if not ctx.stack.insideFunction():
        error("await outside function body")
        return False
    
    for target in stmt.targets:
        targetName = target.name
        if ctx.hasPendingAsync(targetName):
            ctx.resolveAsync(targetName)
        else:
            error("no pending async for '", targetName, "'")
    
    return False

def resolveSpawnStmt(stmt, ctx):
    if not ctx.stack.insideFunction():
        error("spawn outside function body")
        return False
    
    if stmt.target and stmt.target.name != "_":
        ctx.addPendingSpawn(stmt.target.name, stmt.call, stmt.loc)
    
    return False

def resolveJoinStmt(stmt, ctx):
    if not ctx.stack.insideFunction():
        error("join outside function body")
        return False
    
    for target in stmt.targets:
        targetName = target.name
        if ctx.hasPendingSpawn(targetName):
            ctx.resolveSpawn(targetName)
        else:
            error("no pending spawn for '", targetName, "'")
    
    return False

def resolveBlock(block, ctx):
    ctx.pushScope()
    
    # ... resolve statements ...
    
    # Check for unresolved operations
    for name in ctx.getPendingAsyncNames():
        warning("async '", name, "' was never awaited")
    
    for name in ctx.getPendingSpawnNames():
        warning("spawn '", name, "' was never joined")
    
    ctx.popScope()
```

### Complete Example Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    Complete Concurrency Example                             │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Source Code:                                                               │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │  const process () -> int = {                                            ││
│  │      let result string?;               // pendingAsync: {}              ││
│  │      let heavy int?;                   // pendingSpawn: {}              ││
│  │                                                                         ││
│  │      async result = fetchData(url);    // pendingAsync: {result}        ││
│  │      spawn heavy = computeHeavy();     // pendingSpawn: {heavy}         ││
│  │                                                                         ││
│  │      doOtherWork();                    // Still pending                 ││
│  │                                                                         ││
│  │      await result;                     // pendingAsync: {}              ││
│  │      join heavy;                       // pendingSpawn: {}              ││
│  │                                                                         ││
│  │      return result ?? "";                                               ││
│  │  }                                                                      ││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
│  If await/join is missing:                                                  │
│  ┌─────────────────────────────────────────────────────────────────────────┐│
│  │  const process () -> int = {                                            ││
│  │      let result string?;               // pendingAsync: {}              ││
│  │      let heavy int?;                   // pendingSpawn: {}              ││
│  │                                                                         ││
│  │      async result = fetchData(url);    // pendingAsync: {result}        ││
│  │      spawn heavy = computeHeavy();     // pendingSpawn: {heavy}         ││
│  │                                                                         ││
│  │      return 0;                                                          ││
│  │  }  // ← Scope exit:                                                    ││
│  │     // WARNING: async 'result' was never awaited                        ││
│  │     // WARNING: spawn 'heavy' was never joined                          ││
│  └─────────────────────────────────────────────────────────────────────────┘│
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Concurrency Helper Methods

| Method                             | Description                                            |
| ---------------------------------- | ------------------------------------------------------ |
| `addPendingAsync(name, call, loc)` | Add an async operation to the current scope            |
| `addPendingSpawn(name, call, loc)` | Add a spawn operation to the current scope             |
| `hasPendingAsync(name)`            | Check if a name is a pending async operation           |
| `hasPendingSpawn(name)`            | Check if a name is a pending spawn operation           |
| `resolveAsync(name)`               | Remove a pending async operation (resolved by `await`) |
| `resolveSpawn(name)`               | Remove a pending spawn operation (resolved by `join`)  |
| `getPendingAsyncNames()`           | Get all pending async names in the current scope       |
| `getPendingSpawnNames()`           | Get all pending spawn names in the current scope       |
| `hasPendingAsync()`                | Check if there are any pending async operations        |
| `hasPendingSpawn()`                | Check if there are any pending spawn operations        |

---

## Component Interaction

```
┌────────────────────────────────────────────────────────────────────────────┐
│                      Component Interaction Flow                            │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  ┌────────────────────────────────────────────────────────────────────────┐│
│  │  SemaContext (unified context)                                         ││
│  │                                                                        ││
│  │  ┌───────────────────────────────────────────────────────────────────┐ ││
│  │  │  ContextStack                                                     │ ││
│  │  │  ├── m_stack: vector<ContextFrame>                                │ ││
│  │  │  │   ├── kind: ContextKind                                        │ ││
│  │  │  │   ├── node: BaseAST*                                           │ ││
│  │  │  │   └── returnReqs: ReturnRequirements                           │ ││
│  │  │  ├── m_narrowing: vector<NarrowingLevel>                          │ ││
│  │  │  │   ├── narrowedTypes: map<Name, Type*>                          │ ││
│  │  │  │   └── isInverse: bool                                          │ ││
│  │  │  └── methods: insideFunction(), getNarrowedType(), ...            │ ││
│  │  └───────────────────────────────────────────────────────────────────┘ ││
│  │                                                                        ││
│  │  ┌───────────────────────────────────────────────────────────────────┐ ││
│  │  │  Symbol Storage (integrated into SemaContext)                     │ ││
│  │  │  ├── moduleTables: map<Module*, ModuleTable>                      │ ││
│  │  │  │   ├── values: map<Name, ValueDecl*>                            │ ││
│  │  │  │   ├── types: map<Name, TypeDecl*>                              │ ││
│  │  │  │   └── importAliases: map<Name, Module*>                        │ ││
│  │  │  ├── scopes: vector<Scope>                                        │ ││
│  │  │  │   ├── values: map<Name, ValueDecl*>                            │ ││
│  │  │  │   ├── types: map<Name, TypeDecl*>                              │ ││
│  │  │  │   ├── genericParams: map<Name, GenericParam*>                  │ ││
│  │  │  │   ├── pendingAsync: map<Name, PendingAsync>                    │ ││
│  │  │  │   └── pendingSpawn: map<Name, PendingSpawn>                    │ ││
│  │  │  └── methods: lookupValue(), insertValue(), ...                   │ ││
│  │  └───────────────────────────────────────────────────────────────────┘ ││
│  │                                                                        ││
│  │  ┌───────────────────────────────────────────────────────────────────┐ ││
│  │  │  Concurrency Helpers                                              │ ││
│  │  │  ├── addPendingAsync(name, call, loc)                             │ ││
│  │  │  ├── addPendingSpawn(name, call, loc)                             │ ││
│  │  │  ├── hasPendingAsync(name)                                        │ ││
│  │  │  ├── hasPendingSpawn(name)                                        │ ││
│  │  │  ├── resolveAsync(name)                                           │ ││
│  │  │  └── resolveSpawn(name)                                           │ ││
│  │  └───────────────────────────────────────────────────────────────────┘ ││
│  │                                                                        ││
│  └────────────────────────────────────────────────────────────────────────┘│
│                                                                            │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## Summary

| Feature                  | Purpose                                       | Key Methods                                                     |
| ------------------------ | --------------------------------------------- | --------------------------------------------------------------- |
| **Context Tracking**     | Track current semantic construct              | `pushFunction()`, `pop()`, `insideFunction()`                   |
| **Type Narrowing**       | Refine variable types in branches             | `pushNarrowingLevel()`, `narrowVariable()`, `getNarrowedType()` |
| **Return Requirements**  | Track return obligations in curried functions | `advanceReturnGroup()`, `currentReturnGroup()`                  |
| **Self-Reference**       | Detect and validate self-referential types    | `isDefiningType()`, `pushDefiningType()`                        |
| **Symbol Storage**       | Store and lookup declarations                 | `lookupValue()`, `insertValue()`, `lookupType()`                |
| **Concurrency Tracking** | Track pending async/spawn operations          | `addPendingAsync()`, `resolveAsync()`, `addPendingSpawn()`      |