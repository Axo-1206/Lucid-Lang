# Semantic Analysis Structure

The Lucid semantic analyzer (`Sema`) is a multi-pass system that validates, resolves, and annotates the Abstract Syntax Tree (AST). It transforms a syntactically correct AST into a semantically validated one, ready for code generation.

> [!NOTE]
> All code block here are `pseudo code` (or `cpp`), we use the `\```cpp` or `\```swift` for color effects

## File Layout

```
src/sema/
├── Sema.hpp                              # Public API (namespace sema)
├── Sema.cpp                              # analyze() entry point, phase orchestration
│
├── context/
│   ├── SemaContext.hpp                   # Unified context (symbols, type cache, narrowing)
│   ├── SemaContext.cpp
│   ├── ContextStack.hpp/cpp              # Semantic context tracking (func, loop, if, block)
│   └── ModuleTable.hpp/cpp               # Module-level symbol storage
│
├── rules/                                # Analysis rule implementations
│   ├── SemaDecl.cpp                      # Declarations: import, var, func, struct, enum, trait
│   ├── SemaStmt.cpp                      # Statements: if, for, while, switch, return, block
│   ├── SemaExpr.cpp                      # Expressions: literals, binary/unary, calls, pipelines
│   └── SemaConcurrency.cpp               # Concurrency: async, await, spawn, join
│
├── types/
│   ├── SemaType.hpp                      # Main header (includes all sub-headers)
│   ├── SemaResolve.hpp/cpp               # Type resolution (AST → semantic type)
│   ├── SemaCompare.hpp/cpp               # Type equality, assignability, predicates
│   ├── SemaValidate.hpp/cpp              # Semantic validation rules
│   └── SemaLookup.hpp/cpp                # Type lookup helpers
│
├── const_eval/
│   ├── ConstEvaluator.hpp                # Public interface
│   ├── ConstEvaluator.cpp                # Main logic: evaluate, buildDependencyGraph
│   ├── ConstEvalHelpers.hpp              # Internal helper declarations
│   ├── ConstEvalBinary.cpp               # Binary operations (add, sub, mul, div, mod, pow, cmp)
│   ├── ConstEvalUnary.cpp                # Unary operations (neg, not, bitnot)
│   └── ConstEvalStatement.cpp            # Statement execution (block, return, if, while, expr, decl)
│
├── registry/
│   ├── AttributeValidator.hpp/cpp        # Validate attributes (@[export], @[foreign], etc.)
│   └── IntrinsicValidator.hpp/cpp        # Validate intrinsic calls (#sqrt, #memcpy, etc.)
│
└── support/
    ├── SwitchHelpers.hpp/cpp             # Switch exhaustiveness checking
    ├── TypeNarrowHelpers.hpp/cpp         # Type narrowing detection in if conditions
    ├── CaptureAnalysis.hpp/cpp           # Closure capture analysis
    ├── MangledName.hpp/cpp               # Mangled name generation
    └── Helpers.hpp/cpp                   # General helpers
```

## Dispatch Graph

### Program Entry Points (`Sema.cpp`)

The semantic analysis phase is orchestrated by the `analyze()` function,
 which serves as the main entry point for the semantic analyzer. 
 It operates in two distinct passes to ensure correct name resolution and type checking.

**Entry Point Overview**

The `analyze()` function takes a vector of parsed modules and a `SemaContext` 
(which holds all state including symbol tables, type cache, and diagnostics). 
It performs two passes: first registering all top-level names, then resolving types and checking bodies.

#### PHASE 1: Register ALL top-level names (No type resolution)

```cpp
for each module:
│
├── ctx.enterModule(module)
│
└── registerTopLevelNames(module, ctx)
    │
    └── for each decl in module->decls:
        │
        └── registerDeclName(decl, ctx)
            │
            ├── ImportDecl   → registerImportName()
            ├── VarDecl      → registerVarName()
            ├── FuncDecl     → registerFuncName()
            ├── StructDecl   → registerStructName()
            │                    └── registerStructFieldNames()
            ├── EnumDecl     → registerEnumName()
            └── TraitDecl    → registerTraitName()
```
> [!IMPORTANT]
> Phase 1 ONLY registers top-level declarations
> Local variables, parameters, and other scoped names are
> registered during Phase 2 when we actually resolve the bodies

During Phase 1, the analyzer walks through all modules and registers only the names of top-level declarations. 
This phase does not resolve types or inspect function bodies.

**Key Points:**
- Only top-level names are registered (functions, structs, enums, traits, imports)
- No type resolution occurs
- Local variables and parameters are not registered until Phase 2
- Struct field names are registered as part of `registerStructName()`

#### PHASE 2: Resolve ALL types, check bodies, AND evaluate consts

```cpp
for each module:
│
├── ctx.enterModule(module)
│
└── resolveModuleDecls(module, ctx)
    │
    └── for each decl in module->decls:
        │
        └── resolveDecl(decl, ctx)
            │
            ├── If NOT at module level:
            │   ├── VarDecl/FuncDecl  → ctx.insertValue()
            │   └── Struct/Enum/Trait → ctx.insertType()
            │
            └── Dispatch by kind:
                ├── ImportDecl   → resolveImportDecl()
                ├── VarDecl      → resolveVarDecl()
                │                   └── ConstEvaluator::evaluateDecl()
                ├── FuncDecl     → resolveFuncDecl()
                │                   ├── registerParamName()
                │                   ├── registerGenericParamName()
                │                   └── resolveStmt(body)
                ├── StructDecl   → resolveStructDecl()
                │                   ├── resolveTraitRefs()
                │                   └── resolveStructFields()
                ├── EnumDecl     → resolveEnumDecl()
                └── TraitDecl    → resolveTraitDecl()
```

During Phase 2, the analyzer resolves all types, checks function bodies, 
evaluates const expressions, and registers nested declarations.

**Key Points:**
- Types are resolved using `resolveType()`
- Function bodies are checked using `resolveStmt()`
- Const expressions are evaluated using `ConstEvaluator::evaluateDecl()`
- Nested declarations are registered as they are encountered
- All errors are collected in the `DiagnosticEngine`

#### Key Relationships

| Component                 | Responsibility                                | Called From                    |
| ------------------------- | --------------------------------------------- | ------------------------------ |
| `analyze()`               | Orchestrates both phases                      | Compiler driver                |
| `registerTopLevelNames()` | Phase 1 entry point per module                | `analyze()`                    |
| `resolveModuleDecls()`    | Phase 2 entry point per module                | `analyze()`                    |
| `registerDeclName()`      | Dispatches to specific name registrars        | `registerTopLevelNames()`      |
| `resolveDecl()`           | Dispatches to specific declaration resolvers  | `resolveModuleDecls()`         |
| `SemaContext`             | Holds all state (modules, scopes, type cache) | Shared across all functions    |
| `DiagnosticEngine`        | Collects and reports errors                   | Accessed via `ctx.diagnostics` |

---

### Declaration Dispatch (`resolveDecl`)

The `resolveDecl()` function serves as the entry point for resolving individual declarations during Phase 2. It handles both registering nested declarations and dispatching to the appropriate resolver for each declaration kind.

**Purpose:**
- Registers nested declarations (local variables, nested functions, etc.) that were not registered in Phase 1
- Dispatches to specific resolver functions based on the declaration kind
- Coordinates type resolution, body checking, and const evaluation

```cpp
resolveDecl(decl, ctx)                                       [Sema.cpp]
│
├── // PHASE 2 REGISTRATION (Nested declarations only)
│   ┌────────────────────────────────────────────────────────────────────────┐
│   │  IMPORTANT: Top-level declarations are already registered in Phase 1   │
│   │  Only nested declarations are registered during this phase             │
│   └────────────────────────────────────────────────────────────────────────┘
│   │
│   └── if NOT at module level:
│       ├── VarDecl/FuncDecl  → ctx.insertValue(decl->as<ValueDeclAST>())
│       └── Struct/Enum/Trait → ctx.insertType(decl->as<TypeDeclAST>())
│
└── // DISPATCH BY KIND ─────────────────────────────────────────────────────
│
    ├── ImportDecl
    │   └── resolveImportDecl(decl, ctx)                     [SemaDecl.cpp]
    │       ├── Validate imported module exists
    │       └── No further resolution needed // (Phase 1 already registered)
    │
    ├── VarDecl
    │   └── resolveVarDecl(decl, ctx)                        [SemaDecl.cpp]
    │       ├── resolveType(decl->type) → validate type exists
    │       │   // const can't be 'nil' or 'err'
    │       ├── if const: validateConstType() & validateConstInitializer() 
    │       ├── if init: resolveExprWithTarget(init, type) → type check
    │       ├── if const: ConstEvaluator::evaluateDecl()   → evaluate at compile time
    │       └── if let: checkLetSelfReference(init, name)  → prevent self-reference
    │
    ├── FuncDecl
    │   └── resolveFuncDecl(decl, ctx)                       [SemaDecl.cpp]
    │       ├── resolveFuncType(decl->funcType)     → validate function signature
    │       ├── registerGenericParamName() for each generic parameter
    │       │
    │       ├── for each param: resolveParam(param, ctx) → resolve parameter type
    │       │   └── registerParamName(param, ctx) → register in current scope
    │       │
    │       ├── if body: resolveBlock(body, ctx) → analyze function body
    │       │   ├── uses ctx.stack.currentReturnType() for return validation
    │       │   └── uses SymbolScope and ScopedSemanticContext guards
    │       │
    │       ├── analyzeCaptures(func, ctx)                   [CaptureAnalysis.cpp]
    │       │   └── Detect captured variables and validate capture rules
    │       │
    │       └── if '@[foreign]': validateForeignFunction() → ABI & FFI checks
    │
    ├── StructDecl
    │   └── resolveStructDecl(decl, ctx)                     [SemaDecl.cpp]
    │       ├── resolveTraitRefs(traitRefs, ctx)    → resolve each trait reference
    │       ├── registerStructFieldNames(decl, ctx) → Phase 1 already did this
    │       ├── ScopedTypeDefinition(ctx, decl)     → track for self-reference checks
    │       └── resolveStructFields(decl, ctx)      → resolve each field's type
    │           ├── for each field:
    │           │   ├── resolveType(field->type) → validate type exists
    │           │   ├── validateBorrowedContext(field->type, ctx) → Downward Flow
    │           │   └── if const: validateConstType() & validateConstInitializer()
    │           │
    │           └── validateAllTraitImplementations(structDecl, ctx)
    │               ├── validateTraitImplementation() for each trait
    │               └── checkTraitFieldConflicts() → detect conflicting fields
    │
    ├── EnumDecl
    │   └── resolveEnumDecl(decl, ctx)                       [SemaDecl.cpp]
    │       ├── resolveType(decl->backingType) → validate backing type exists
    │       ├── check backing type is integer primitive
    │       └── for each variant:
    │           ├── check value uniqueness → no duplicate values
    │           └── ensure value fits in backing type
    │
    └── TraitDecl
        └── resolveTraitDecl(decl, ctx)                      [SemaDecl.cpp]
            ├── registerGenericParamName() for each generic parameter
            └── for each field:
                ├── resolveType(field->type) → validate type exists
                └── validateBorrowedContext(field->type, ctx) → Downward Flow
```

**Key Relationships:**

| Component                        | Responsibility                 | Called From            |
| -------------------------------- | ------------------------------ | ---------------------- |
| `resolveDecl()`                  | Main dispatch entry point      | `resolveModuleDecls()` |
| `resolveVarDecl()`               | Resolves variable declarations | `resolveDecl()`        |
| `resolveFuncDecl()`              | Resolves function declarations | `resolveDecl()`        |
| `resolveStructDecl()`            | Resolves struct declarations   | `resolveDecl()`        |
| `resolveEnumDecl()`              | Resolves enum declarations     | `resolveDecl()`        |
| `resolveTraitDecl()`             | Resolves trait declarations    | `resolveDecl()`        |
| `resolveParam()`                 | Resolves function parameters   | `resolveFuncDecl()`    |
| `resolveTraitRefs()`             | Resolves trait references      | `resolveStructDecl()`  |
| `resolveStructFields()`          | Resolves struct field types    | `resolveStructDecl()`  |
| `analyzeCaptures()`              | Detects closure captures       | `resolveFuncDecl()`    |
| `ConstEvaluator::evaluateDecl()` | Evaluates const declarations   | `resolveVarDecl()`     |

**Important Rules:**
- **Nested Registration**: Only nested declarations are registered in Phase 2 (top-level already registered)
- **Const Validation**: Const declarations must be definite (non-nullable, non-fallible) and have an initializer
- **Self-Reference Check**: `let` variables cannot reference themselves in their initializer
- **Downward Flow Rule**: Borrowed types (`&T`, `[_]T`) cannot appear in struct fields, array elements, or function returns
- **Trait Implementation**: Structs must implement all fields of their traits, with no conflicts

---

### Statement Resolution (`resolveStmt`) — `SemaStmt.cpp`

The `resolveStmt()` function is the main entry point for resolving and validating statements during Phase 2. It dispatches to specific statement resolvers and manages control flow analysis.

**Purpose:**
- Validates statement semantics (e.g., `break` inside loops, `return` inside functions)
- Performs type checking on expressions within statements
- Manages control flow analysis (detects unreachable code, return paths)
- Handles type narrowing for `if` statements and concurrency statements

```cpp
resolveStmt(stmt, ctx)
│
├── // DISPATCH BY KIND ─────────────────────────────────────────────────────
│
    ├── BlockStmt
    │   └── resolveBlock(stmt, ctx)
    │       ├── if pending inverse narrowing: apply it (from standalone if)
    │       │   ├── ctx.stack.pushNarrowingLevel(true)
    │       │   └── ctx.stack.narrowVariable() for each narrowed variable
    │       │
    │       ├── SymbolScope(ctx) → create new lexical scope
    │       ├── ScopedSemanticContext(ctx, Block) → push block context
    │       │
    │       ├── for each stmt:
    │       │   ├── if transfers: warning (unreachable code)
    │       │   └── transfers = resolveStmt(stmt, ctx)
    │       │
    │       ├── check for unresolved async/spawn operations
    │       │   ├── getPendingAsyncNames() → warn: unawaited async
    │       │   └── getPendingSpawnNames() → warn: unjoined spawn
    │       └── return transfers (true if block guarantees control transfer)
    │
    ├── IfStmt
    │   └── resolveIfStmt(stmt, ctx)
    │       ├── ScopedSemanticContext(ctx, IfStmt) → push if context
    │       ├── ScopedIfCondition(ctx, hasElse) → enable narrowing detection
    │       ├── resolveExprWithTarget(condition, boolType) → type check
    │       │
    │       ├── CONST EVALUATION: evaluate condition at compile time
    │       │   ├── if condition is const true: only resolve then branch
    │       │   └── if condition is const false: only resolve else branch
    │       │
    │       ├── info = ctx.stack.getPendingNarrowing() → capture narrowing info
    │       │
    │       ├── THEN BRANCH:
    │       │   ├── SymbolScope(ctx) → new scope
    │       │   ├── if hasNarrowing && !info.isEquality:
    │       │   │   └── ScopedNarrowing(ctx, info.narrowings, false)
    │       │   └── thenReturns = resolveStmt(thenBranch, ctx)
    │       │
    │       ├── ELSE BRANCH:
    │       │   ├── SymbolScope(ctx) → new scope
    │       │   ├── if hasNarrowing && info.isEquality:
    │       │   │   └── ScopedNarrowing(ctx, info.narrowings, true)
    │       │   └── elseReturns = resolveStmt(elseBranch, ctx)
    │       │
    │       ├── if no else and thenReturns:
    │       │   └── ctx.stack.setPendingInverseNarrowing(info) → store for block
    │       └── return thenReturns && elseReturns
    │
    ├── SwitchStmt
    │   └── resolveSwitchStmt(stmt, ctx)
    │       ├── resolveExprWithTarget(subject) → type check subject
    │       ├── isValidSwitchType(subjectType) → integer, enum, bool, char, string
    │       ├── ScopedSemanticContext(ctx, SwitchBody) → push switch context
    │       │
    │       ├── CONST EVALUATION: evaluate subject at compile time
    │       │
    │       ├── for each case:
    │       │   ├── for each value: resolveExprWithTarget(value, subjectType)
    │       │   ├── isSwitchCaseCompatible(value, subjectType) → validate
    │       │   ├── duplicate case detection:
    │       │   │   ├── literal values → check uniqueness
    │       │   │   ├── enum variants → check uniqueness
    │       │   │   └── ranges → check overlap
    │       │   └── resolveBlock(case->body) → resolve case body
    │       │
    │       ├── if isEnumType(subjectType):
    │       │   └── switch_helpers::checkExhaustiveness() → ensure all variants covered
    │       │
    │       ├── if defaultBody: resolveBlock(defaultBody) → resolve default
    │       └── return allCasesReturn && (defaultBody || !isEnumType)
    │
    ├── ForStmt
    │   └── resolveForStmt(stmt, ctx)
    │       ├── ScopedSemanticContext(ctx, LoopBody) → push loop context
    │       ├── SymbolScope(ctx) → new scope for loop variables
    │       │
    │       ├── if range loop (no valueVar):
    │       │   ├── resolveRangeExpr(range) → validate range bounds
    │       │   ├── CONST EVALUATION: validate range at compile time
    │       │   │   ├── evaluateAsInt(lo) & evaluateAsInt(hi)
    │       │   │   ├── if inclusive: lo <= hi
    │       │   │   └── if exclusive: lo < hi
    │       │   ├── if indexVar: ctx.insertValue(indexVar) → register index
    │       │   └── if step: resolveExprWithTarget(step, intType)
    │       │
    │       ├── if collection loop (has valueVar):
    │       │   ├── resolveExpr(iterable) → resolve collection
    │       │   ├── if indexVar: ctx.insertValue(indexVar) → register index
    │       │   ├── if valueVar: ctx.insertValue(valueVar) → register value
    │       │   └── validate value type matches iterable element type
    │       │
    │       ├── resolveBlock(body) → resolve loop body
    │       └── return false (loops dont guarantee return unless break/return)
    │
    ├── WhileStmt
    │   └── resolveWhileStmt(stmt, ctx)
    │       ├── ScopedSemanticContext(ctx, LoopBody) → push loop context
    │       ├── resolveExprWithTarget(condition, boolType) → type check
    │       │
    │       ├── CONST EVALUATION: check if condition is compile-time constant
    │       │   ├── if condition is const false → warning (body unreachable)
    │       │   └── if condition is const true → warning (infinite loop)
    │       │
    │       ├── resolveBlock(body) → resolve loop body
    │       └── return false (loops dont guarantee return)
    │
    ├── ReturnStmt
    │   └── resolveReturnStmt(stmt, ctx)
    │       ├── if !ctx.stack.insideFunction() → error (return outside function)
    │       ├── expectedType = ctx.stack.currentReturnType()
    │       │
    │       ├── if stmt->value:
    │       │   ├── if !expectedType → error (return value in void function)
    │       │   ├── resolveExprWithTarget(value, expectedType) → type check
    │       │   ├── markClosureIfEscaping(value, ctx) → detect closure return
    │       │   │   └── if returning closure: mark as escaping (heap-allocated)
    │       │   ├── if valueState == Err && !isFallibleType(expectedType) → error
    │       │   └── if valueState == Nil && !isNullableType(expectedType) → error
    │       ├── else:
    │       │   └── if expectedType → error (missing return value)
    │       │
    │       └── return true (return guarantees control transfer)
    │
    ├── BreakStmt
    │   └── resolveBreakStmt(stmt, ctx)
    │       ├── if !ctx.stack.insideLoop() && !ctx.stack.insideSwitch() → error
    │       └── return true (break guarantees control transfer)
    │
    ├── ContinueStmt
    │   └── resolveContinueStmt(stmt, ctx)
    │       ├── if !ctx.stack.insideLoop() → error
    │       └── return true (continue guarantees control transfer)
    │
    ├── ExprStmt
    │   └── resolveExprStmt(stmt, ctx)
    │       ├── resolveExpr(expr) → resolve and type check expression
    │       ├── if expr returns value and has no side effects:
    │       │   └── warning: discarded result (unused value)
    │       ├── if expr is intrinsic void:
    │       │   └── validate statement context
    │       └── return false (expression statements dont transfer control)
    │
    ├── DeclStmt
    │   └── resolveDeclStmt(stmt, ctx)
    │       └── resolveDecl(decl, ctx) → recursive call to resolve declaration
    │           └── return false (declarations dont transfer control)
    │
    ├── AsyncStmt
    │   └── resolveAsyncStmt(stmt, ctx)                        [SemaConcurrency.cpp]
    │       ├── if !ctx.stack.insideFunction() → error
    │       ├── resolveType(binding->type) → must be FutureTypeAST
    │       ├── ctx.insertValue(binding) → register binding
    │       ├── resolveExprWithTarget(call, innerType) → call must return inner type
    │       └── ctx.addPendingAsync(name, call, loc) → register for await
    │
    ├── AwaitStmt
    │   └── resolveAwaitStmt(stmt, ctx)                        [SemaConcurrency.cpp]
    │       ├── if !ctx.stack.insideFunction() → error
    │       │
    │       ├── for each target:
    │       │   ├── if hasPendingAsync(targetName):
    │       │   │   ├── unwrap FutureTypeAST → get innerType
    │       │   │   ├── ctx.stack.narrowVariable(targetName, innerType) → narrow type
    │       │   │   └── ctx.resolveAsync(targetName) → remove from pending
    │       │   └── else: error (not a pending async)
    │       └── return false
    │
    ├── SpawnStmt
    │   └── resolveSpawnStmt(stmt, ctx)                        [SemaConcurrency.cpp]
    │       ├── if !ctx.stack.insideFunction() → error
    │       ├── if binding:
    │       │   ├── resolveType(binding->type) → must be ThreadTypeAST
    │       │   ├── ctx.insertValue(binding) → register binding
    │       │   ├── resolveExprWithTarget(call, innerType) → call must return inner type
    │       │   └── ctx.addPendingSpawn(name, call, loc) → register for join
    │       └── else (discard pattern): resolveExpr(call) → fire-and-forget
    │
    └── JoinStmt
        └── resolveJoinStmt(stmt, ctx)                         [SemaConcurrency.cpp]
            ├── if !ctx.stack.insideFunction() → error
            │
            ├── for each target:
            │   ├── if hasPendingSpawn(targetName):
            │   │   ├── unwrap ThreadTypeAST → get innerType
            │   │   ├── ctx.stack.narrowVariable(targetName, innerType) → narrow type
            │   │   └── ctx.resolveSpawn(targetName) → remove from pending
            │   └── else: error (not a pending spawn)
            └── return false
```

**Key Relationships:**

| Statement Type | Primary Responsibility                         | Key Helper                              |
| -------------- | ---------------------------------------------- | --------------------------------------- |
| `BlockStmt`    | Scoping, pending narrowing application         | `SymbolScope`, `ScopedSemanticContext`  |
| `IfStmt`       | Type narrowing, const evaluation               | `ScopedIfCondition`, `ScopedNarrowing`  |
| `SwitchStmt`   | Exhaustiveness, duplicate case detection       | `switch_helpers::checkExhaustiveness()` |
| `ForStmt`      | Range/collection iteration, const validation   | `ConstEvaluator::evaluateAsInt()`       |
| `WhileStmt`    | Loop condition, const evaluation               | `ConstEvaluator::evaluate()`            |
| `ReturnStmt`   | Return type checking, closure escape detection | `markClosureIfEscaping()`               |
| `AsyncStmt`    | Future type registration                       | `ctx.addPendingAsync()`                 |
| `AwaitStmt`    | Future type narrowing                          | `ctx.stack.narrowVariable()`            |
| `SpawnStmt`    | Thread type registration                       | `ctx.addPendingSpawn()`                 |
| `JoinStmt`     | Thread type narrowing                          | `ctx.stack.narrowVariable()`            |

**Important Rules:**
- **Control Flow Analysis**: Each statement returns `true` if it guarantees control transfer (return, break, continue)
- **Type Narrowing**: If statements capture narrowing info from conditions and apply to branches
- **Pending Inverse Narrowing**: Standalone if with early exit stores narrowing for the enclosing block
- **Const Evaluation**: Conditions and ranges are evaluated at compile time for optimization and validation
- **Concurrency Tracking**: Async/spawn operations are tracked in the scope and must be resolved (awaited/joined)

---

### Expression Resolution (`resolveExprWithTarget`) — `SemaExpr.cpp`

The `resolveExprWithTarget()` function is the main entry point for resolving and type-checking expressions during Phase 2. It validates expressions against an optional target type and stores the resolved type directly on the AST node.

**Purpose:**
- Resolves the type of every expression in the AST
- Validates expressions against expected types (if provided)
- Performs operator validation and type promotion
- Detects type narrowing patterns in conditions
- Stores resolved type, value state, l-value status, and const status

```cpp
resolveExprWithTarget(expr, targetType, ctx)
│
├── // DISPATCH BY KIND ─────────────────────────────────────────────────────
│
│   ┌── LiteralExpr ─────────────────────────────────────────────────────────┐
│   │  resolveLiteralExpr(expr, targetType, ctx)                             │
│   │  ├── True/False → boolType, ValueState::Definite, isLValue=false       │
│   │  ├── Int/Hex/Binary → intType (or target if specified)                 │
│   │  ├── Float → floatType (or target if specified)                        │
│   │  ├── String → stringType, ValueState::Definite                         │
│   │  ├── Char → charType, ValueState::Definite                             │
│   │  ├── Nil → targetType if nullable, else UnknownType                    │
│   │  └── Err → targetType if fallible, else UnknownType                    │
│   └────────────────────────────────────────────────────────────────────────┘
│
│   ┌── IdentifierExpr ──────────────────────────────────────────────────────┐
│   │  resolveIdentifierExpr(expr, targetType, ctx)                          │
│   │  ├── if name == "_": discard placeholder (UnknownType)                 │
│   │  ├── if isGenericParam(name): error (type param used as value)         │
│   │  ├── lookupValue(name) → ValueDeclAST                                  │
│   │  │   └── if not found: error (undefined value)                         │
│   │  ├── if pending future: error (await/join first)                       │
│   │  ├── if isCaptured && isBorrowedType(declType): error                  │
│   │  │   └── closures cannot capture &T or [_]T                            │
│   │  ├── if genericArgs: validateGenericArguments()                        │
│   │  │   └── validate function instantiation                               │
│   │  ├── set isLValue, isConst from declaration keyword                    │
│   │  │   ├── let → isLValue=true, isConst=false                            │
│   │  │   └── const → isLValue=false, isConst=true                          │
│   │  └── apply type narrowing from ContextStack                            │
│   │      └── if getNarrowedType(name) → use narrowed type                  │
│   └────────────────────────────────────────────────────────────────────────┘
│
│   ┌── ArrayLiteralExpr ────────────────────────────────────────────────────┐
│   │  resolveArrayLiteralExpr(expr, targetType, ctx)                        │
│   │  ├── if empty: use targetType element type or UnknownType              │
│   │  ├── resolve first element → firstType                                 │
│   │  ├── for each element: resolveExprWithTarget(elem, firstType)          │
│   │  │   └── if type mismatch: error (array elements must have same type)  │
│   │  └── getArrayType(kind, size, elementType) → cached array type         │
│   └────────────────────────────────────────────────────────────────────────┘
│
│   ┌── StructLiteralExpr ───────────────────────────────────────────────────┐
│   │  resolveStructLiteralExpr(expr, targetType, ctx)                       │
│   │  ├── lookupType(typeName) → StructDeclAST                              │
│   │  │   └── if not found or not struct: error                             │
│   │  ├── validateGenericArguments() → validate generic args                │
│   │  ├── for each field init:                                              │
│   │  │   ├── lookup field in struct → FieldDeclAST                         │
│   │  │   ├── if const field: cannot assign nil/err                         │
│   │  │   ├── resolveExprWithTarget(value, field->type)                     │
│   │  │   └── if function field: must be function value                     │
│   │  └── check missing required fields                                     │
│   │      ├── if field has default or nullable/fallible → optional          │
│   │      └── else: error (missing required field)                          │
│   └────────────────────────────────────────────────────────────────────────┘
│
│   ┌── BinaryExpr ─────────────────────────────────────────────────────────┐
│   │  resolveBinaryExpr(expr, targetType, ctx)                             │
│   │  ├── resolve left and right operands                                  │
│   │  ├── if in if condition: detectNarrowingPattern()                     │
│   │  │   └── if found: return boolType (narrowing info stored)            │
│   │  ├── // Arithmetic (Add, Sub, Mul, Div, Pow, Mod) ───────────────────│
│   │  │   ├── reject nullable/fallible operands (must narrow first)        │
│   │  │   ├── if numeric: promote int → float if mixed                     │
│   │  │   │   └── getLargerIntegerType() for promotion                     │
│   │  │   └── else: error (arithmetic requires numeric operands)           │
│   │  ├── // Comparison (Eq, Ne, Lt, Gt, Le, Ge) ─────────────────────────│
│   │  │   ├── if numeric: allow mixed types (promotion)                    │
│   │  │   ├── if non-numeric: must be same type                            │
│   │  │   └── return boolType                                              │
│   │  ├── // Logical (And, Or) ───────────────────────────────────────────│
│   │  │   ├── reject nullable/fallible operands (must narrow first)        │
│   │  │   ├── require bool operands                                        │
│   │  │   └── return boolType                                              │
│   │  └── // Bitwise (BitAnd, BitOr, BitXor, Shl, Shr) ───────────────────│
│   │      ├── reject nullable/fallible operands (must narrow first)        │
│   │      ├── require integer operands                                     │
│   │      └── promote to larger integer type                               │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── UnaryExpr ──────────────────────────────────────────────────────────┐
│   │  resolveUnaryExpr(expr, targetType, ctx)                              │
│   │  ├── resolve operand                                                  │
│   │  ├── Neg: require numeric, reject nullable/fallible                   │
│   │  ├── Not: require bool, reject nullable/fallible                      │
│   │  └── BitNot: require integer, reject nullable/fallible                │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── CallExpr ───────────────────────────────────────────────────────────┐
│   │  resolveCallExpr(expr, targetType, ctx)                               │
│   │  ├── resolveExpr(callee) → must be FuncTypeAST                        │
│   │  ├── resolveCalleeOrError() → FuncDeclAST                             │
│   │  │   └── handles IdentifierExpr and ModuleAccessExpr                  │
│   │  ├── validateGenericArguments() if generic function                   │
│   │  ├── check arg count with variadic support:                           │
│   │  │   ├── requiredArgs = params before variadic                        │
│   │  │   ├── if hasVariadic: args >= requiredArgs                         │
│   │  │   └── if no variadic: args == param count                          │
│   │  ├── for each arg: resolveExprWithTarget(arg, expectedType)           │
│   │  │   ├── if arg is Err → must be fallible expected type               │
│   │  │   └── if arg is Nil → must be nullable expected type               │
│   │  └── return funcType->returnType                                      │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── IntrinsicCallExpr ──────────────────────────────────────────────────┐
│   │  resolveIntrinsicCallExpr(expr, targetType, ctx)                      │
│   │  ├── validateIntrinsicCall(expr, ctx)                                 │
│   │  │   ├── validateScopeExit() registers callback                       │
│   │  │   └── validate argument count and types                            │
│   │  ├── if void: return nullptr (statement-only intrinsic)               │
│   │  ├── getIntrinsicReturnType(expr, targetType, ctx)                    │
│   │  └── set IntrinsicRegistry::llvmID for code generation                │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── IndexExpr ──────────────────────────────────────────────────────────┐
│   │  resolveIndexExpr(expr, targetType, ctx)                              │
│   │  ├── resolveExpr(target) → must be ArrayTypeAST                       │
│   │  ├── reject nullable/fallible target (must narrow first)              │
│   │  ├── resolveExprWithTarget(index, intType) → index must be integer    │
│   │  ├── isLValue = target->isLValue (propagate l-value)                  │
│   │  └── return arrayType->element                                        │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── SliceExpr ──────────────────────────────────────────────────────────┐
│   │  resolveSliceExpr(expr, targetType, ctx)                              │
│   │  ├── resolveExpr(target) → must be ArrayTypeAST                       │
│   │  ├── reject nullable/fallible target (must narrow first)              │
│   │  ├── resolve start/end against intType (optional)                     │
│   │  ├── result is always [_]T (slice)                                    │
│   │  └── isLValue=false (slices are never l-values)                       │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── FieldAccessExpr ────────────────────────────────────────────────────┐
│   │  resolveFieldAccessExpr(expr, targetType, ctx)                        │
│   │  ├── resolveExpr(object) → NamedTypeAST                               │
│   │  ├── reject nullable/fallible object (must narrow first)              │
│   │  ├── if generic type: isFieldAccessibleOnGenericType()                │
│   │  │   └── check trait constraints for field access                     │
│   │  ├── lookupType(name) → StructDeclAST or EnumDeclAST                  │
│   │  ├── find field in struct fields or enum variants                     │
│   │  └── return field->type                                               │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── ModuleAccessExpr ───────────────────────────────────────────────────┐
│   │  resolveModuleAccessExpr(expr, targetType, ctx)                       │
│   │  ├── lookupValueByAlias(moduleName, memberName)                       │
│   │  │   └── uses ctx.lookupImport() to find module                       │
│   │  ├── check isValueExported() → must have @[export]                    │
│   │  ├── if generic: validateGenericArguments()                           │
│   │  └── return decl->type                                                │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── NullCoalesceExpr ───────────────────────────────────────────────────┐
│   │  resolveNullCoalesceExpr(expr, targetType, ctx)                       │
│   │  ├── resolveExpr(value) → must be nullable or fallible                │
│   │  ├── unwrapNullable/Fallible() → get inner type                       │
│   │  ├── resolveExprWithTarget(fallback, innerType)                       │
│   │  └── return innerType (or fallback type)                              │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── AssignExpr ─────────────────────────────────────────────────────────┐
│   │  resolveAssignExpr(expr, targetType, ctx)                             │
│   │  ├── resolveExpr(lhs) → get lhsType                                   │
│   │  ├── check lhs->isLValue → must be assignable                         │
│   │  ├── check !lhs->isConst → cannot assign to const                     │
│   │  ├── if compound assignment:                                          │
│   │  │   ├── reject nullable/fallible LHS (must narrow first)             │
│   │  │   └── validate operator type (numeric or integer)                  │
│   │  └── return lhsType                                                   │
│   └───────────────────────────────────────────────────────────────────────┘
│
│   ┌── PipelineExpr ──────────────────────────────────────────────────────┐
│   │  resolvePipelineExpr(expr, targetType, ctx)                          │
│   │  ├── resolveExpr(seed) → get input type                              │
│   │  └── for each step: resolvePipelineStep(step, currentType)           │
│   │      ├── resolveExpr(callable) → FuncTypeAST                         │
│   │      ├── check first param matches inputType                         │
│   │      └── currentType = funcType->returnType                          │
│   └──────────────────────────────────────────────────────────────────────┘
│
│   ┌── ComposeExpr ───────────────────────────────────────────────────────┐
│   │  resolveComposeExpr(expr, targetType, ctx)                           │
│   │  ├── resolveComposeOperand(left) → FuncTypeAST                       │
│   │  └── for each operand:                                               │
│   │      ├── resolveComposeOperand(operand) → FuncTypeAST                │
│   │      ├── check prev output → next input assignable                   │
│   │      └── currentFunc = nextFunc                                      │
│   └──────────────────────────────────────────────────────────────────────┘
│
│   ┌── AnonFuncExpr ──────────────────────────────────────────────────────┐
│   │  resolveAnonFuncExpr(expr, targetType, ctx)                          │
│   │  ├── resolveFuncType(funcType) → validate signature                  │
│   │  ├── pushScope() → new scope for parameters                          │
│   │  ├── for each param: resolveParam(param, ctx)                        │
│   │  │   └── registerParamName(param, ctx)                               │
│   │  ├── ctx.stack.pushAnonFunction(expr, returnType)                    │
│   │  ├── resolveBlock(body) → analyze body                               │
│   │  ├── ctx.stack.pop() → pop function context                          │
│   │  ├── analyzeCaptures(expr, ctx) → detect captures                    │
│   │  │   ├── walk AST for identifier references                          │
│   │  │   ├── check if variable is from outer scope                       │
│   │  │   └── validate borrowed types cannot be captured                  │
│   │  └── return funcType                                                 │
│   └──────────────────────────────────────────────────────────────────────┘
│
│   ┌── IfExpr ────────────────────────────────────────────────────────────┐
│   │  resolveIfExpr(expr, targetType, ctx)                                │
│   │  ├── resolveExprWithTarget(condition, boolType)                      │
│   │  ├── resolveExpr(thenBranch) → thenType                              │
│   │  ├── resolveExpr(elseBranch) → elseType                              │
│   │  ├── check isAssignable(thenType, elseType)                          │
│   │  └── return thenType                                                 │
│   └──────────────────────────────────────────────────────────────────────┘
│
│   ┌── RangeExpr ─────────────────────────────────────────────────────────┐
│   │  resolveRangeExpr(expr, targetType, ctx)                             │
│   │  ├── resolveExprWithTarget(lo, intType)                              │
│   │  ├── resolveExprWithTarget(hi, intType)                              │
│   │  ├── if inclusive: lo <= hi                                          │
│   │  └── return loType (or intType)                                      │
│   └──────────────────────────────────────────────────────────────────────┘
│
├── // VALIDATE AGAINST TARGET TYPE ──────────────────────────────────────
│   └── if targetType && !isAssignable(targetType, result, ctx):
│       └── error (type mismatch)
│
└── // STORE RESULT ON AST NODE ──────────────────────────────────────────
    ├── expr->resolvedType = result
    ├── expr->valueState = ValueState (Definite/Nil/Err/Unknown)
    ├── expr->isLValue = boolean (can be assigned to)
    └── expr->isConst = boolean (compile-time constant)
```

**Key Relationships:**

| Expression Type     | Primary Responsibility               | Key Helper                                             |
| ------------------- | ------------------------------------ | ------------------------------------------------------ |
| `LiteralExpr`       | Literal type inference               | `ctx.getBoolType()`, `ctx.getIntType()`, etc.          |
| `IdentifierExpr`    | Name resolution, type narrowing      | `lookupValue()`, `ctx.stack.getNarrowedType()`         |
| `ArrayLiteralExpr`  | Element type consistency             | `typesEqual()`                                         |
| `StructLiteralExpr` | Field validation, defaults           | `resolveType()`, `lookupType()`                        |
| `BinaryExpr`        | Operator validation, type promotion  | `isNumericType()`, `getLargerIntegerType()`            |
| `UnaryExpr`         | Operator validation                  | `isNumericType()`, `isBoolType()`                      |
| `CallExpr`          | Callee resolution, argument checking | `resolveCalleeOrError()`, `validateGenericArguments()` |
| `IntrinsicCallExpr` | Intrinsic validation                 | `validateIntrinsicCall()`, `getIntrinsicReturnType()`  |
| `IndexExpr`         | Array indexing, l-value propagation  | `isArrayType()`                                        |
| `SliceExpr`         | Slice creation                       | `getArrayType(Slice, 0, element)`                      |
| `FieldAccessExpr`   | Field lookup                         | `lookupType()`, `isFieldAccessibleOnGenericType()`     |
| `ModuleAccessExpr`  | Cross-module access                  | `lookupValueByAlias()`, `isValueExported()`            |
| `NullCoalesceExpr`  | Nil/err fallback                     | `unwrapNullable()`, `unwrapFallible()`                 |
| `AssignExpr`        | L-value validation                   | `isLValue`, `isConst`                                  |
| `PipelineExpr`      | Function composition                 | `resolvePipelineStep()`                                |
| `ComposeExpr`       | Function composition                 | `resolveComposeOperand()`                              |
| `AnonFuncExpr`      | Closure creation                     | `resolveFuncType()`, `analyzeCaptures()`               |
| `IfExpr`            | Branch type compatibility            | `isAssignable()`                                       |
| `RangeExpr`         | Range validation                     | `evaluateAsInt()`                                      |

**Important Rules:**
- **Target Type Validation**: Expressions are validated against the expected type if provided
- **Type Narrowing**: Binary expressions in if conditions can detect narrowing patterns
- **L-Value Propagation**: Index expressions inherit l-value status from their target
- **Const Evaluation**: Const expressions are evaluated at compile time and marked as `isConst`
- **Value State**: Tracks whether an expression is `Definite`, `Nil`, `Err`, or `Unknown`
- **Capture Validation**: Closures cannot capture borrowed types (`&T`, `[_]T`)

---

### Type Resolution (`resolveType`) — `SemaResolve.cpp`

The `resolveType()` function is the main entry point for resolving type annotations to their semantic representations. It walks the type AST and converts each type node into a fully resolved semantic type, performing validation and generic argument resolution along the way.

**Purpose:**
- Resolves every type annotation to a semantic representation
- Validates that referenced types exist and are accessible
- Resolves generic arguments and validates constraints
- Enforces the Downward Flow Rule for borrowed types
- Caches resolved types for fast equality comparison

```cpp
resolveType(type, ctx)
│
├── // DISPATCH BY KIND ───────────────────────────────────────────────────────
│
│   ┌── PrimitiveTypeAST ──────────────────────────────────────────────────────┐
│   │  resolvePrimitiveType(type, ctx)                                         │
│   │  ├── Primitive types are built-in and always valid                       │
│   │  └── return type (no validation needed)                                  │
│   └──────────────────────────────────────────────────────────────────────────┘
│
│   ┌── NamedTypeAST ──────────────────────────────────────────────────────────┐
│   │  resolveNamedType(type, ctx)                                             │
│   │  │                                                                       │
│   │  ├── Step 1: Check if this is a generic parameter                        │
│   │  │   └── if ctx.isGenericParam(name): return type (generic param)        │
│   │  │                                                                       │
│   │  ├── Step 2: Look up as concrete type                                    │
│   │  │   └── ctx.lookupType(name) → TypeDeclAST                              │
│   │  │       ├── StructDeclAST  → struct type                                │
│   │  │       ├── EnumDeclAST    → enum type                                  │
│   │  │       └── TraitDeclAST   → trait type                                 │
│   │  │                                                                       │
│   │  ├── Step 3: If not found → error (undefined type)                       │
│   │  │                                                                       │
│   │  ├── Step 4: Validate generic arguments if present                       │
│   │  │   ├── Check arity matches declaration parameters                      │
│   │  │   ├── Resolve each generic argument type                              │
│   │  │   └── validateGenericArguments(args, params, useSite)                 │
│   │  │       ├── Check constraints (trait bounds)                            │
│   │  │       └── Ensure arguments are not borrowed types                     │
│   │  │                                                                       │
│   │  └── Step 5: Return cached NamedTypeAST                                  │
│   │      └── ctx.getNamedType(name) → canonicalized type pointer             │
│   └──────────────────────────────────────────────────────────────────────────┘
│
│   ┌── ModuleTypeAccessAST ───────────────────────────────────────────────────┐
│   │  resolveModuleTypeAccess(type, ctx)                                      │
│   │  │                                                                       │
│   │  ├── Step 1: Look up the module alias                                    │
│   │  │   └── ctx.lookupImport(moduleName) → ModuleAST                        │
│   │  │       └── if not found: error (module not imported)                   │
│   │  │                                                                       │
│   │  ├── Step 2: Look up the type in the module's table                      │
│   │  │   └── ctx.lookupModuleTypeMember(module, memberName)                  │
│   │  │       └── if not found: error (type not found in module)              │
│   │  │                                                                       │
│   │  ├── Step 3: Check if the type is exported                               │
│   │  │   └── if !ctx.isTypeExported(decl): error (private member)            │
│   │  │                                                                       │
│   │  ├── Step 4: Validate generic arguments if present                       │
│   │  │   ├── Check if type is generic (has params)                           │
│   │  │   ├── Check arity matches                                             │
│   │  │   ├── Resolve each generic argument type                              │
│   │  │   └── validateGenericArguments(args, params, useSite)                 │
│   │  │                                                                       │
│   │  └── Step 5: Return resolved NamedTypeAST                                │
│   │      └── ctx.getNamedType(memberName) → canonicalized type pointer       │
│   └──────────────────────────────────────────────────────────────────────────┘
│
│   ┌── ArrayTypeAST ──────────────────────────────────────────────────────────┐
│   │  resolveArrayType(type, ctx)                                             │
│   │  │                                                                       │
│   │  ├── Step 1: Resolve the element type                                    │
│   │  │   └── resolveType(type->element) → must succeed                       │
│   │  │                                                                       │
│   │  ├── Step 2: Validate element type                                       │
│   │  │   ├── if element is RefTypeAST: error (reference in array)            │
│   │  │   │   └── Downward Flow Rule: &T cannot be stored                     │
│   │  │   └── if element is ArrayTypeAST (slice): error (slice in array)      │
│   │  │       └── Downward Flow Rule: [_]T cannot be stored                   │
│   │  │                                                                       │
│   │  └── Step 3: Validate context if this is a slice type                    │
│   │      └── if type->isSlice(): validateBorrowedContext(type, ctx)          │
│   └──────────────────────────────────────────────────────────────────────────┘
│
│   ┌── NullableTypeAST ───────────────────────────────────────────────────────┐
│   │  resolveNullableType(type, ctx)                                          │
│   │  │                                                                       │
│   │  ├── Step 1: Resolve the inner type                                      │
│   │  │   └── resolveType(type->inner) → must succeed                         │
│   │  │                                                                       │
│   │  ├── Step 2: Validate inner type cannot be function                      │
│   │  │   └── if inner is FuncTypeAST: error (function cannot be nullable)    │
│   │  │                                                                       │
│   │  └── Step 3: Validate inner type cannot be array                         │
│   │      └── if inner is ArrayTypeAST: error (array cannot be nullable)      │
│   └──────────────────────────────────────────────────────────────────────────┘
│
│   ┌── FallibleTypeAST ───────────────────────────────────────────────────────┐
│   │  resolveFallibleType(type, ctx)                                          │
│   │  │                                                                       │
│   │  ├── Step 1: Resolve the inner type                                      │
│   │  │   └── resolveType(type->inner) → must succeed                         │
│   │  │                                                                       │
│   │  ├── Step 2: Validate inner type cannot be function                      │
│   │  │   └── if inner is FuncTypeAST: error (function cannot be fallible)    │
│   │  │                                                                       │
│   │  └── Step 3: Validate inner type cannot be array                         │
│   │      └── if inner is ArrayTypeAST: error (array cannot be fallible)      │
│   └──────────────────────────────────────────────────────────────────────────┘
│
│   ┌── CombinedTypeAST ───────────────────────────────────────────────────────┐
│   │  resolveCombinedType(type, ctx)                                          │
│   │  │                                                                       │
│   │  ├── Step 1: Resolve the inner type                                      │
│   │  │   └── resolveType(type->inner) → must succeed                         │
│   │  │                                                                       │
│   │  ├── Step 2: Validate inner type cannot be function                      │
│   │  │   └── if inner is FuncTypeAST: error (function cannot be combined)    │
│   │  │                                                                       │
│   │  └── Step 3: Validate inner type cannot be array                         │
│   │      └── if inner is ArrayTypeAST: error (array cannot be combined)      │
│   └──────────────────────────────────────────────────────────────────────────┘
│
│   ┌── RefTypeAST ────────────────────────────────────────────────────────────┐
│   │  resolveRefType(type, ctx)                                               │
│   │  │                                                                       │
│   │  ├── Step 1: Resolve the inner type                                      │
│   │  │   └── resolveType(type->inner) → must succeed                         │
│   │  │                                                                       │
│   │  ├── Step 2: Validate inner type cannot be trait                         │
│   │  │   └── if isTraitType(inner, ctx): error (&Trait not allowed)          │
│   │  │                                                                       │
│   │  └── Step 3: Apply Downward Flow Rule                                    │
│   │      └── validateBorrowedContext(type, ctx)                              │
│   │          ├── if in struct field: error (&T cannot be stored)             │
│   │          ├── if in array element: error (&T cannot be stored)            │
│   │          ├── if in function return: error (&T cannot escape upward)      │
│   │          └── if in closure capture: error (&T cannot be captured)        │
│   └──────────────────────────────────────────────────────────────────────────┘
│
│   ┌── PtrTypeAST ────────────────────────────────────────────────────────────┐
│   │  resolvePtrType(type, ctx)                                               │
│   │  │                                                                       │
│   │  ├── Step 1: Resolve the inner type                                      │
│   │  │   └── resolveType(type->inner) → must succeed                         │
│   │  │                                                                       │
│   │  └── Step 2: Return type (always valid structurally)                     │
│   │      └── Raw pointers are sealed conduits - FFI checks done separately   │
│   └──────────────────────────────────────────────────────────────────────────┘
│
│   ┌── FuncTypeAST ───────────────────────────────────────────────────────────┐
│   │  resolveFuncType(type, ctx)                                              │
│   │  │                                                                       │
│   │  ├── Step 1: Resolve all parameter types                                 │
│   │  │   └── for each param: resolveType(param->type) → must succeed         │
│   │  │                                                                       │
│   │  ├── Step 2: Resolve return type (if present)                            │
│   │  │   └── resolveType(type->returnType) → must succeed                    │
│   │  │                                                                       │
│   │  ├── Step 3: Validate return type cannot be borrowed                     │
│   │  │   ├── if isBorrowedType(returnType): error                            │
│   │  │   │   └── Downward Flow Rule: &T and [_]T cannot escape upward        │
│   │  │   └── if returnType is RefTypeAST: error                              │
│   │  │                                                                       │
│   │  ├── Step 4: Validate return type cannot be trait                        │
│   │  │   └── if isTraitType(returnType, ctx): error (trait cannot be returned)│
│   │  │                                                                       │
│   │  └── Step 5: Recursively resolve curried return type                     │
│   │      └── if returnType is FuncTypeAST: resolveFuncType(returnType)       │
│   └──────────────────────────────────────────────────────────────────────────┘
│
├── // ERROR HANDLING ─────────────────────────────────────────────────────────
│   └── On any error: report via ctx.diagnostics.error() and return nullptr
│
└── // RETURN VALUE CONVENTIONS ──────────────────────────────────────────────
    ├── nullptr: Type does not exist (hard error)
    │   ├── Type lookup failed (undeclared)
    │   ├── Generic arity mismatch
    │   └── Module/type not found
    │
    └── TypeAST*: Type exists (valid or invalid)
        ├── PrimitiveTypeAST: Built-in type
        ├── NamedTypeAST: User-defined type (cached)
        ├── ArrayTypeAST: Array type (cached)
        ├── NullableTypeAST: T? (cached)
        ├── FallibleTypeAST: T! (cached)
        ├── CombinedTypeAST: T?! (cached)
        ├── RefTypeAST: &T (cached)
        ├── PtrTypeAST: *T (cached)
        └── FuncTypeAST: Function type (cached)
```

**Key Relationships:**

| Type Kind             | Primary Responsibility                  | Key Helper                                           |
| --------------------- | --------------------------------------- | ---------------------------------------------------- |
| `PrimitiveTypeAST`    | Built-in type validation                | None (always valid)                                  |
| `NamedTypeAST`        | User-defined type lookup, generics      | `ctx.lookupType()`, `ctx.getNamedType()`             |
| `ModuleTypeAccessAST` | Cross-module type access                | `ctx.lookupImport()`, `ctx.lookupModuleTypeMember()` |
| `ArrayTypeAST`        | Element type validation, Downward Flow  | `validateBorrowedContext()`                          |
| `NullableTypeAST`     | Inner type validation (no funcs/arrays) | `resolveType()`                                      |
| `FallibleTypeAST`     | Inner type validation (no funcs/arrays) | `resolveType()`                                      |
| `CombinedTypeAST`     | Inner type validation (no funcs/arrays) | `resolveType()`                                      |
| `RefTypeAST`          | Reference validation, Downward Flow     | `validateBorrowedContext()`                          |
| `PtrTypeAST`          | Pointer validation (structural only)    | None (always valid structurally)                     |
| `FuncTypeAST`         | Parameter/return validation, currying   | `resolveType()`, `validateBorrowedContext()`         |

**Important Rules:**

- **Generic Parameter Priority**: Generic parameters are checked FIRST before concrete type lookup
- **Type Canonicalization**: All resolved types are cached for fast pointer equality comparison
- **Downward Flow Rule**: Borrowed types (`&T`, `[_]T`) cannot be stored in structs, arrays, returned from functions, or captured by closures
- **Function Types Cannot Be Nullable**: `(T) -> R?` is not allowed—use `?(T) -> R` instead
- **Array Types Cannot Be Nullable**: `[N]T?` is not allowed—use nullable element type instead
- **Trait Types Cannot Be Returned**: Functions cannot return traits—use concrete struct types
- **Foreign Functions**: Raw pointers (`*T`) are the only way to cross the FFI boundary with mutable data

---

### Capture Analysis (`CaptureAnalysis.cpp`)

Capture analysis detects which variables from outer scopes are referenced inside a closure (anonymous function or nested function). It validates that captured variables follow the language's safety rules and stores the capture information for code generation.

**Purpose:**
- Detects which variables are captured by closures and nested functions
- Validates that captured variables are not borrowed types (`&T`, `[_]T`)
- Validates that captured variables are not linear types (`Future<T>`, `Thread<T>`)
- Stores capture information on the AST node for code generation
- Marks closures as escaping when they are returned from functions

```cpp
analyzeCaptures(expr, ctx)                             // Analyze anonymous function
│
├── // EARLY EXIT ──────────────────────────────────────────────────────────────
│   └── if expr->body is null: return
│
├── // COLLECT CAPTURES ────────────────────────────────────────────────────────
│   │
│   └── collectCaptures(body, ctx, expr)
│       │
│       └── // WALK AST ────────────────────────────────────────────────────────
│           │
│           ├── // IdentifierExpr ─────────────────────────────────────────────
│           │   │   Check if this identifier is a capture
│           │   │
│           │   ├── if isGenericParam(name) → NOT CAPTURE
│           │   │   └── Generic parameters are resolved at instantiation time
│           │   │
│           │   ├── if isInCurrentScope(name) → NOT CAPTURE
│           │   │   └── Variables declared in the same function are local
│           │   │
│           │   ├── if isModuleMember(name) → NOT CAPTURE
│           │   │   └── Module members are global, not captured
│           │   │
│           │   └── else → CAPTURE
│           │       └── Variable from an outer function scope
│           │
│           ├── // Nested AnonFuncExpr ────────────────────────────────────────
│           │   │   Recursively analyze nested closures
│           │   │
│           │   └── analyzeCaptures(nested, ctx)
│           │       └── Nested closures have their own capture list
│           │
│           ├── // FuncDecl ────────────────────────────────────────────────────
│           │   └── skip (captures analyzed separately)
│           │
│           └── // Other AST nodes ─────────────────────────────────────────────
│               └── recurse into children (expressions, statements, etc.)
│
├── // VALIDATE CAPTURES ──────────────────────────────────────────────────────
│   │
│   └── for each capture:
│       │
│       ├── // Rule 1: No borrowed types ─────────────────────────────────────
│       │   └── if isBorrowedType(decl->type):
│       │       ├── error: closure cannot capture borrowed type
│       │       ├── ctx.diagnostics.error(DiagCode::Sem_InvalidCapture)
│       │       │   └── "closure cannot capture borrowed type 'x' (&T or [_]T)"
│       │       ├── ctx.diagnostics.note()
│       │       │   └── "Only owned values can be captured by closures"
│       │       └── return (error already reported)
│       │
│       ├── // Rule 2: No linear types ──────────────────────────────────────
│       │   └── if decl->type is FutureTypeAST or ThreadTypeAST:
│       │       ├── error: closure cannot capture linear type
│       │       ├── ctx.diagnostics.error(DiagCode::Sem_InvalidCapture)
│       │       │   └── "closure cannot capture Future<T> or Thread<T>"
│       │       └── return (error already reported)
│       │
│       └── // Add to capture list ──────────────────────────────────────────
│           └── expr->captures.push_back(capture)
│
└── // STORE RESULT ────────────────────────────────────────────────────────────
    └── expr->hasClosure = !expr->captures.empty()
```

```cpp
markClosureIfEscaping(expr, ctx)                     // Detect escaping closures
│
├── // PURPOSE ─────────────────────────────────────────────────────────────────
│   │   Detects when a closure is returned from a function and must be
│   │   heap-allocated because it outlives the function call.
│   │
│   └── Escaping closures are heap-allocated; non-escaping closures are stack-allocated
│
├── // CASE 1: IdentifierExpr ──────────────────────────────────────────────────
│   │   Returning a named function or closure variable
│   │
│   ├── lookupValue(name) → FuncDeclAST
│   │   └── if not found: return (not a function)
│   │
│   ├── if func is nested (closureDepth > 0) and is const:
│   │   └── func->isEscaping = true
│   │       └── Nested const function returned → escapes
│   │
│   └── if func is AnonFuncExpr and is const:
│       └── func->isEscaping = true
│           └── Anonymous function returned → escapes
│
└── // CASE 2: AnonFuncExpr ────────────────────────────────────────────────────
    │   Directly returning an anonymous function literal
    │
    └── expr->isEscaping = true
        └── Always escapes when returned
```

**Capture Detection Walkthrough:**

```cpp
┌────────────────────────────────────────────────────────────────────────────────┐
│                      Capture Detection Example                                 │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │  const makeCounter (start int) -> (int) -> int = {                      │   │
│  │      let count int = start                                              │   │
│  │                                                                         │   │
│  │      return (step int) -> int {                                         │   │
│  │          count += step                                                  │   │
│  │          return count                                                   │   │
│  │      }                                                                  │   │
│  │  }                                                                      │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │  Step 1: analyzeCaptures() called on the inner anonymous function       │   │
│  │                                                                         │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐    │   │
│  │  │  Walk AST of the anonymous function body:                       │    │   │
│  │  │                                                                 │    │   │
│  │  │  ├── BinaryExpr (count += step)                                 │    │   │
│  │  │  │   ├── left: IdentifierExpr("count")                          │    │   │
│  │  │  │   │   ├── isInCurrentScope("count")? → false                 │    │   │
│  │  │  │   │   ├── isModuleMember("count")? → false                   │    │   │
│  │  │  │   │   └── → CAPTURE                                          │    │   │
│  │  │  │   │       └── count is captured (from outer scope)           │    │   │
│  │  │  │   │                                                          │    │   │
│  │  │  │   └── right: IdentifierExpr("step")                          │    │   │
│  │  │  │       ├── isInCurrentScope("step")? → true (parameter)       │    │   │
│  │  │  │       └── → NOT CAPTURE                                      │    │   │
│  │  │  │                                                              │    │   │
│  │  │  └── ReturnStmt (return count)                                  │    │   │
│  │  │      └── IdentifierExpr("count")                                │    │   │
│  │  │          ├── isInCurrentScope("count")? → false                 │    │   │
│  │  │          └── → CAPTURE (already captured)                       │    │   │
│  │  └─────────────────────────────────────────────────────────────────┘    │   │
│  │                                                                         │   │
│  │  Step 2: Validate captures                                              │   │
│  │  └── count is not borrowed type → valid capture                         │   │
│  │                                                                         │   │
│  │  Step 3: Store result                                                   │   │
│  │  └── expr->captures = count, expr->hasClosure = true                    │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │  Step 4: markClosureIfEscaping() called on return expression            │   │
│  │  └── expr is AnonFuncExpr → expr->isEscaping = true                     │   │
│  │      └── Closure escapes → heap-allocated in code generation            │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

**Key Relationships:**

| Component                 | Responsibility                                  | Called From                                  |
| ------------------------- | ----------------------------------------------- | -------------------------------------------- |
| `analyzeCaptures()`       | Main entry point for capture analysis           | `resolveAnonFuncExpr()`, `resolveFuncDecl()` |
| `collectCaptures()`       | Walks AST to find identifier references         | `analyzeCaptures()`                          |
| `isInCurrentScope()`      | Checks if variable is in current function scope | `collectCaptures()`                          |
| `isModuleMember()`        | Checks if variable is a module-level global     | `collectCaptures()`                          |
| `isBorrowedType()`        | Validates captured type is owned                | `analyzeCaptures()`                          |
| `markClosureIfEscaping()` | Detects escaping closures for heap allocation   | `resolveReturnStmt()`                        |

**Important Rules:**

| Rule                      | Description                                                     | Example                                                                          |
| ------------------------- | --------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| **No Borrowed Types**     | Closures cannot capture `&T` or `[_]T`                          | `let x &int = ...; return (y int) -> int { return *x + y }` ❌                    |
| **No Linear Types**       | Closures cannot capture `Future<T>` or `Thread<T>`              | `async result int = fetch(); return () -> int { return await result }` ❌         |
| **Module Members**        | Module-level globals are not captured (they're global)          | `const PI f64 = 3.14; return (r f64) -> f64 { return PI * r * r }` ✅             |
| **Generic Parameters**    | Generic parameters are not captured (resolved at instantiation) | `<T> const id (v T) -> (T) -> T = { return (x T) -> T { return x } }` ✅          |
| **Local Variables**       | Variables from outer function scopes are captured               | `let count int = 0; return () -> int { return count }` ✅                         |
| **Escaping Closures**     | Closures returned from functions are heap-allocated             | `return (x int) -> int { return x + 1 }` → heap allocated                        |
| **Non-Escaping Closures** | Closures used locally are stack-allocated                       | `let f (int) -> int = (x int) -> int { return x + 1 }; use(f)` → stack allocated |

**Capture Storage on AST Nodes:**

```cpp
// AnonFuncExprAST fields for capture analysis
struct AnonFuncExprAST {
    // ...
    bool hasClosure = false;                    // true if captures any variables
    std::vector<CapturedVariable> captures;     // list of captured variables
    bool isEscaping = false;                    // true if returned from function
};

// FuncDeclAST fields for capture analysis
struct FuncDeclAST {
    // ...
    bool hasClosure = false;                    // true if nested function captures
    std::vector<CapturedVariable> captures;     // list of captured variables
    bool isEscaping = false;                    // true if returned from function
};

// CapturedVariable structure
struct CapturedVariable {
    InternedString name;                        // variable name
    ValueDeclAST* decl;                         // variable declaration
    TypeAST* type;                              // variable type
    bool isMutable;                             // true if variable is let (mutable)
    bool isCapturedByRef;                       // true if captured by reference
};
```

**Key Implementation Notes:**

1. **Capture Detection**: The analyzer walks the AST recursively, checking every `IdentifierExpr` node to determine if it references a variable from an outer scope.

2. **Scope Determination**: A variable is considered a capture if it is:
   - Not a generic parameter
   - Not in the current scope (local variable or parameter)
   - Not a module member (top-level declaration)

3. **Validation Rules**: Captures are validated immediately when detected:
   - Borrowed types (`&T`, `[_]T`) are rejected with a clear error message
   - Linear types (`Future<T>`, `Thread<T>`) are rejected

4. **Escape Analysis**: `markClosureIfEscaping()` is called during return statement resolution to detect closures that outlive the function call and must be heap-allocated.

5. **Nested Closures**: Nested closures are analyzed recursively, with each closure maintaining its own capture list.

---

### Type Narrowing (`TypeNarrowHelpers.cpp`)

Type narrowing is a flow-sensitive analysis that refines variable types 
based on conditional checks and operations. It allows the compiler to understand 
that after certain checks, a variable's type is more specific than its declared type.

```swift
┌───────────────────────────────────────────────────────────────────────────────┐
│                         Type Narrowing Overview                               │
├───────────────────────────────────────────────────────────────────────────────┤
│                                                                               │
│  ┌─────────────────────────────────────────────────────────────────────────┐  │
│  │                    Narrowing Entry Points                               │  │
│  ├─────────────────────────────────────────────────────────────────────────┤  │
│  │                                                                         │  │
│  │  ┌─────────────────────┐    ┌─────────────────────┐                     │  │
│  │  │   If Statement      │    │   Await/Join        │                     │  │
│  │  │   (Condition)       │    │   Statements        │                     │  │
│  │  ├─────────────────────┤    ├─────────────────────┤                     │  │
│  │  │ x != nil → direct   │    │ await x → Future<T> │                     │  │
│  │  │ x == nil → inverse  │    │   narrows to T      │                     │  │
│  │  │ x != err → direct   │    │ join x → Thread<T>  │                     │  │
│  │  │ x == err → inverse  │    │   narrows to T      │                     │  │
│  │  └─────────────────────┘    └─────────────────────┘                     │  │
│  │                                                                         │  │
│  └─────────────────────────────────────────────────────────────────────────┘  │
│                                                                               │
└───────────────────────────────────────────────────────────────────────────────┘
```

#### 1. NARROWING ENTRY POINTS

The narrowing system is triggered from two main entry points:

```cpp
┌───────────────────────────────────────────────────────────────────────────────┐
│                        Narrowing Entry Points                                 │
├───────────────────────────────────────────────────────────────────────────────┤
│                                                                               │
│  ┌─────────────────────────────────────────────────────────────────────────┐  │
│  │                      IF STATEMENT ENTRY                                 │  │
│  ├─────────────────────────────────────────────────────────────────────────┤  │
│  │                                                                         │  │
│  │  resolveIfStmt()                                                        │  │
│  │  │                                                                      │  │
│  │  ├── ScopedIfCondition(ctx)  // Sets isIfConditionCtx = true            │  │
│  │  │                                                                      │  │
│  │  ├── resolveExprWithTarget(condition, boolType)                         │  │
│  │  │   │                                                                  │  │
│  │  │   └── resolveBinaryExpr()                                            │  │
│  │  │       │                                                              │  │
│  │  │       └── if ctx.stack.isIfConditionCtx():                           │  │
│  │  │           │                                                          │  │
│  │  │           └── info = detectNarrowingPattern(expr, ctx)               │  │
│  │  │               │                                                      │  │
│  │  │               └── ctx.stack.setPendingNarrowing(info)                │  │
│  │  │                                                                      │  │
│  │  ├── info = ctx.stack.getPendingNarrowing()                             │  │
│  │  │                                                                      │  │
│  │  ├── THEN BRANCH:                                                       │  │
│  │  │   └── ScopedNarrowing(ctx, info.narrowings, false)                   │  │
│  │  │       └── pushNarrowingLevel(false)                                  │  │
│  │  │           └── narrowVariable(name, type)  // Apply each narrowing    │  │
│  │  │                                                                      │  │
│  │  └── ELSE BRANCH:                                                       │  │
│  │      └── ScopedNarrowing(ctx, info.narrowings, true)                    │  │
│  │          └── pushNarrowingLevel(true)                                   │  │
│  │              └── narrowVariable(name, type)  // Apply each narrowing    │  │
│  │                                                                         │  │
│  └─────────────────────────────────────────────────────────────────────────┘  │
│                                                                               │
│  ┌─────────────────────────────────────────────────────────────────────────┐  │
│  │                    AWAIT/JOIN STATEMENT ENTRY                           │  │
│  ├─────────────────────────────────────────────────────────────────────────┤  │
│  │                                                                         │  │
│  │  resolveAwaitStmt() / resolveJoinStmt()                                 │  │
│  │  │                                                                      │  │
│  │  └── for each target:                                                   │  │
│  │      │                                                                  │  │
│  │      ├── if hasPendingAsync(targetName):                                │  │
│  │      │   │                                                              │  │
│  │      │   ├── futureType = decl->type->as<FutureTypeAST>()               │  │
│  │      │   ├── innerType = futureType->inner                              │  │
│  │      │   │                                                              │  │
│  │      │   └── ctx.stack.narrowVariable(targetName, innerType)            │  │
│  │      │       │                                                          │  │
│  │      │       └── pushNarrowingLevel(false)                              │  │
│  │      │           └── narrowedTypes[targetName] = innerType              │  │
│  │      │                                                                  │  │
│  │      └── ctx.resolveAsync(targetName)  // Remove from pending           │  │
│  │                                                                         │  │
│  └─────────────────────────────────────────────────────────────────────────┘  │
│                                                                               │
└───────────────────────────────────────────────────────────────────────────────┘
```

#### 2. CONDITION NARROWING EXTRACTION

The core narrowing detection happens in `extractNarrowingsFromCondition()`, which analyzes condition expressions to extract type narrowing information.

```cpp
extractNarrowingsFromCondition(expr, ctx, outIsValidMixed)
│
├── // 1. Handle `or` at top level ────────────────────────────────────────
│   Pattern: a == nil or b == nil
│   ├── left = extractNarrowingsFromCondition(left)
│   ├── right = extractNarrowingsFromCondition(right)
│   ├── If both have narrowing and different isEquality → invalid (reject)
│   └── Merge both narrowings (OR combines both possibilities)
│
├── // 2. Handle `and` at top level ───────────────────────────────────────
│   Pattern: a == nil and b == nil
│   └── Return empty (no narrowing - unsound because inverse would be OR)
│
├── // 3. Handle simple binary comparison ─────────────────────────────────
│   └── detectSingleNarrowing(expr)
│       ├── x == nil → narrows x to inner type, isEquality = true
│       ├── x != nil → narrows x to inner type, isEquality = false
│       ├── x == err → narrows x to inner type, isEquality = true
│       └── x != err → narrows x to inner type, isEquality = false
│
└── // 4. Handle `not x` ──────────────────────────────────────────────────
    Pattern: not x
    └── Inverse narrowing: x is nil/false, isEquality = true

detectSingleNarrowing(binary, ctx)
│
├── if binary->op != Eq and binary->op != Ne → return empty
│
├── Pattern: (identifier == nil) or (identifier != nil)
│   └── if left is IdentifierExpr and right is LiteralExpr:
│       └── detectIdentifierNarrowing(info, id, lit, isEquality, ctx)
│
└── Pattern: (nil == identifier) or (err == identifier)  // Reverse order
    └── if left is LiteralExpr and right is IdentifierExpr:
        └── detectIdentifierNarrowing(info, id, lit, isEquality, ctx)

detectIdentifierNarrowing(info, id, lit, isEquality, ctx)
│
├── if lit->kind != Nil and lit->kind != Err → return
│
├── decl = ctx.lookupValue(id->name)
│   if !decl → return
│
├── if !isNullableType(decl->type) and !isFallibleType(decl->type) → return
│
├── innerType = getInnerType(decl, ctx)
│   if !innerType → return
│
└── info.hasNarrowing = true
    info.isEquality = isEquality
    info.narrowings[id->name] = innerType
```

#### 3. NARROWING PATTERNS

The narrowing system supports three distinct patterns:

```cpp
┌──────────────────────────────────────────────────────────────────────────────┐
│                         Narrowing Patterns                                   │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐   │
│  │                    Pattern 1: Nullable/ Fallible Checks               │   │
│  ├───────────────────────────────────────────────────────────────────────┤   │
│  │                                                                       │   │
│  │  ┌────────────────────────────────────────────────────────────────┐   │   │
│  │  │  x != nil   → Direct Narrowing (applies to THEN branch)        │   │   │
│  │  │  ┌─────────────────────────────────────────────────────────┐   │   │   │
│  │  │  │  let x int? = getValue()                                │   │   │   │
│  │  │  │  if x != nil {                                          │   │   │   │
│  │  │  │      // x is int (narrowed from int?)                   │   │   │   │
│  │  │  │      use(x) // safe                                     │   │   │   │
│  │  │  │  }                                                      │   │   │   │
│  │  │  │  // x is int? (narrowing level popped)                  │   │   │   │
│  │  │  └─────────────────────────────────────────────────────────┘   │   │   │
│  │  └────────────────────────────────────────────────────────────────┘   │   │
│  │                                                                       │   │
│  │  ┌────────────────────────────────────────────────────────────────┐   │   │
│  │  │  x == nil   → Inverse Narrowing (applies to ELSE or rest)      │   │   │
│  │  │  ┌─────────────────────────────────────────────────────────┐   │   │   │
│  │  │  │  let x int? = getValue()                                │   │   │   │
│  │  │  │  if x == nil {                                          │   │   │   │
│  │  │  │      return                                             │   │   │   │
│  │  │  │  }                                                      │   │   │   │
│  │  │  │  // x is int (pending inverse narrowing applied)        │   │   │   │
│  │  │  │  use(x) // safe                                         │   │   │   │
│  │  │  └─────────────────────────────────────────────────────────┘   │   │   │
│  │  └────────────────────────────────────────────────────────────────┘   │   │
│  │                                                                       │   │
│  │  ┌────────────────────────────────────────────────────────────────┐   │   │
│  │  │  x != err  → Direct Narrowing (applies to THEN branch)         │   │   │
│  │  │  x == err  → Inverse Narrowing (applies to ELSE or rest)       │   │   │
│  │  └────────────────────────────────────────────────────────────────┘   │   │
│  │                                                                       │   │
│  └───────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐   │
│  │                    Pattern 2: Combined Conditions                     │   │
│  ├───────────────────────────────────────────────────────────────────────┤   │
│  │                                                                       │   │
│  │  ┌────────────────────────────────────────────────────────────────┐   │   │
│  │  │  x != nil and y != nil  → All != checks (direct narrowing)     │   │   │
│  │  │  ┌─────────────────────────────────────────────────────────┐   │   │   │
│  │  │  │  if x != nil and y != nil {                             │   │   │   │
│  │  │  │      // x is int, y is string (both narrowed)           │   │   │   │
│  │  │  │  }                                                      │   │   │   │
│  │  │  └─────────────────────────────────────────────────────────┘   │   │   │
│  │  └────────────────────────────────────────────────────────────────┘   │   │
│  │                                                                       │   │
│  │  ┌────────────────────────────────────────────────────────────────┐   │   │
│  │  │  x == nil or y == nil   → All == checks (inverse narrowing)    │   │   │
│  │  │  ┌─────────────────────────────────────────────────────────┐   │   │   │
│  │  │  │  if x == nil or y == nil {                              │   │   │   │
│  │  │  │      return                                             │   │   │   │
│  │  │  │  }                                                      │   │   │   │
│  │  │  │  // x is int OR y is string (both narrowed)             │   │   │   │
│  │  │  └─────────────────────────────────────────────────────────┘   │   │   │
│  │  └────────────────────────────────────────────────────────────────┘   │   │
│  │                                                                       │   │
│  │  ┌────────────────────────────────────────────────────────────────┐   │   │
│  │  │  x != nil and y == nil  → ❌ REJECTED (mixed operators)        │   │   │
│  │  │  ┌─────────────────────────────────────────────────────────┐   │   │   │   
│  │  │  │  if x != nil and y == nil {                             │   │   │   │   
│  │  │  │      // ERROR: mixed '!=' and '==' in condition         │   │   │   │   
│  │  │  │      // Cannot determine narrowing semantics            │   │   │   │   
│  │  │  │  }                                                      │   │   │   │   
│  │  │  └─────────────────────────────────────────────────────────┘   │   │   │   
│  │  └────────────────────────────────────────────────────────────────┘   │   │
│  │                                                                       │   │
│  └───────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐   │
│  │                    Pattern 3: Await/Join Narrowing                    │   │
│  ├───────────────────────────────────────────────────────────────────────┤   │
│  │                                                                       │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐  │   │
│  │  │  await x   → Narrow Future<T> → T (applies to rest of block)    │  │   │
│  │  │  ┌──────────────────────────────────────────────────────────┐   │  │   │
│  │  │  │  async result int = fetchValue() // result is Future<int>│   │  │   │
│  │  │  │  await result                    // Narrow to int        │   │  │   │
│  │  │  │  // result is int here                                   │   │  │   │
│  │  │  │  use(result) // safe                                     │   │  │   │
│  │  │  └──────────────────────────────────────────────────────────┘   │  │   │
│  │  └─────────────────────────────────────────────────────────────────┘  │   │
│  │                                                                       │   │
│  │  ┌────────────────────────────────────────────────────────────────┐   │   │
│  │  │  join x    → Narrow Thread<T> → T (applies to rest of block)   │   │   │
│  │  │  ┌─────────────────────────────────────────────────────────┐   │   │   │
│  │  │  │  spawn result int = computeValue() // result is Thread<int> │   │   │
│  │  │  │  join result                     // Narrow to int       │   │   │   │
│  │  │  │  // result is int here                                  │   │   │   │
│  │  │  │  use(result) // safe                                    │   │   │   │
│  │  │  └─────────────────────────────────────────────────────────┘   │   │   │
│  │  └────────────────────────────────────────────────────────────────┘   │   │
│  │                                                                       │   │
│  └───────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└──────────────────────────────────────────────────────────────────────────────┘
```

#### 4. NARROWING STACK MANAGEMENT

The narrowing stack tracks flow-sensitive type refinements across nested scopes.

```swift
┌────────────────────────────────────────────────────────────────────────────────┐
│                        Narrowing Stack Structure                               │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │  Level 3 (innermost)  ──────────────────────────────────────┐           │   │
│  │  { x: int, y: string }                                      │           │   │
│  │                                                             │           │   │
│  │  Level 2              ──────────────────────────────────────│──┐        │   │
│  │  { x: int? }                                                │  │        │   │
│  │                                                             │  │        │   │
│  │  Level 1              ──────────────────────────────────────│──│──┐     │   │
│  │  { }                                                        │  │  │     │   │
│  │                                                             │  │  │     │   │
│  │                                                             │  │  │     │   │
│  │  Lookup "x" ────────────────────────────────────────────────┘  │  │     │   │
│  │    → Level 3 has x → returns int                               │  │     │   │
│  │                                                                │  │     │   │
│  │  Lookup "y" ───────────────────────────────────────────────────┘  │     │   │
│  │    → Level 3 has y → returns string                               │     │   │
│  │                                                                   │     │   │
│  │  Lookup "z" ──────────────────────────────────────────────────────┘     │   │
│  │    → No level has z → returns nullptr                                   │   │
│  │                                                                         │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

```cpp
ContextStack Narrowing Methods:
│
├── pushNarrowingLevel(bool isInverse)
│   └── Creates new NarrowingLevel on the stack
│
├── popNarrowingLevel()
│   └── Removes the innermost NarrowingLevel
│
├── narrowVariable(name, type)
│   └── Adds variable narrowing to the current (innermost) level
│
├── getNarrowedType(name)
│   └── Searches stack from innermost to outermost for a narrowed type
│       └── Returns first match, or nullptr if none found
│
└── isNarrowingInverse()
    └── Returns true if the current level is inverse narrowing
```

#### 5. PENDING INVERSE NARROWING

For standalone if statements with early exit (`if x == nil { return }`), the inverse narrowing is stored as pending and applied to the rest of the block.

```cpp
┌───────────────────────────────────────────────────────────────────────────────┐
│                      Pending Inverse Narrowing Flow                           │
├───────────────────────────────────────────────────────────────────────────────┤
│                                                                               │
│  ┌────────────────────────────────────────────────────────────────────────┐   │
│  │  Step 1: Condition Analysis                                            │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐   │   │
│  │  │  if x == nil {                                                  │   │   │
│  │  │      return                                                     │   │   │
│  │  │  }                                                              │   │   │
│  │  │  // Pending inverse narrowing captured: x → int                 │   │   │
│  │  └─────────────────────────────────────────────────────────────────┘   │   │
│  └────────────────────────────────────────────────────────────────────────┘   │
│                                                                               │
│  ┌────────────────────────────────────────────────────────────────────────┐   │
│  │  Step 2: Store Pending Inverse Narrowing                               │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐   │   │
│  │  │  if !stmt->elseBranch && thenReturns &&                         │   │   │
│  │  │     hasNarrowing && info.isEquality {                           │   │   │
│  │  │      ctx.stack.setPendingInverseNarrowing(info)                 │   │   │
│  │  │  }                                                              │   │   │
│  │  └─────────────────────────────────────────────────────────────────┘   │   │
│  └────────────────────────────────────────────────────────────────────────┘   │
│                                                                               │
│  ┌────────────────────────────────────────────────────────────────────────┐   │
│  │  Step 3: Apply Pending Narrowing on Block Entry                        │   │
│  │  ┌─────────────────────────────────────────────────────────────────┐   │   │
│  │  │  resolveBlock() {                                               │   │   │
│  │  │      if ctx.stack.hasPendingInverseNarrowing() {                │   │   │
│  │  │          info = ctx.stack.getPendingInverseNarrowing()          │   │   │
│  │  │          ctx.stack.pushNarrowingLevel(true)                     │   │   │
│  │  │          for (name, type : info.narrowings) {                   │   │   │
│  │  │              ctx.stack.narrowVariable(name, type)               │   │   │
│  │  │          }                                                      │   │   │
│  │  │          ctx.stack.clearPendingInverseNarrowing()               │   │   │
│  │  │      }                                                          │   │   │
│  │  │      // ... resolve block statements ...                        │   │   │
│  │  │  }                                                              │   │   │
│  │  └─────────────────────────────────────────────────────────────────┘   │   │
│  └────────────────────────────────────────────────────────────────────────┘   │
│                                                                               │
└───────────────────────────────────────────────────────────────────────────────┘
```

#### 6. EFFECTIVE TYPE LOOKUP

When resolving an identifier, the compiler checks the narrowing stack for a narrowed type before falling back to the declaration's type.

```cpp
getEffectiveType(decl, name)  [SemaContext]
│
├── // Check if there's a narrowed type for this variable
├── narrowedType = stack.getNarrowedType(name)
├── if narrowedType:
│   └── return narrowedType
│
└── // No narrowing active - use the declaration's type
    return decl->type

Usage in resolveIdentifierExpr():
│
└── TypeAST* effectiveType = ctx.getEffectiveType(decl, name)
    └── Returns narrowed type if available, otherwise decl->type
```

#### 7. NARROWING RULES SUMMARY

| Pattern                 | isEquality | Direction  | Effect                        |
| ----------------------- | ---------- | ---------- | ----------------------------- |
| `x != nil`              | `false`    | Direct     | `x: T?` → `T` in THEN branch  |
| `x == nil`              | `true`     | Inverse    | `x: T?` → `T` in ELSE/rest    |
| `x != err`              | `false`    | Direct     | `x: T!` → `T` in THEN branch  |
| `x == err`              | `true`     | Inverse    | `x: T!` → `T` in ELSE/rest    |
| `await x`               | `true`     | Direct     | `x: Future<T>` → `T` in rest  |
| `join x`                | `true`     | Direct     | `x: Thread<T>` → `T` in rest  |
| `x != nil and y != nil` | `false`    | Direct     | Both variables narrowed       |
| `x == nil or y == nil`  | `true`     | Inverse    | Both variables narrowed       |
| `x != nil and y == nil` | N/A        | ❌ REJECTED | Mixed operators not supported |
| `x == nil or y != nil`  | N/A        | ❌ REJECTED | Mixed operators not supported |
| `not x`                 | `true`     | Inverse    | `x` narrowed (nil/false)      |



### Const Evaluation (`ConstEvaluator`) — `ConstEvaluator.cpp`

The const evaluator is responsible for evaluating expressions at compile-time. 
It is invoked during Phase 2 of semantic analysis when resolving `const` declarations. 
The evaluator handles both constant variables and constant functions, with support 
for compile-time execution of functions.

```swift
┌─────────────────────────────────────────────────────────────────────────────────┐
│                          ConstEvaluator Overview                                │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│  ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────────┐  │
│  │   Constant Values   │  │   Control Flow      │  │   Type System           │  │
│  ├─────────────────────┤  ├─────────────────────┤  ├─────────────────────────┤  │
│  │ • Bool (true/false) │  │ • if/else           │  │ • Int → int64           │  │
│  │ • Int (int64)       │  │ • while loops       │  │ • Float → double        │  │
│  │ • Float (double)    │  │ • for loops (range) │  │ • String                │  │
│  │ • String            │  │ • switch/case       │  │ • Struct                │  │
│  │ • Char              │  │ • return            │  │ • Array                 │  │
│  │ • Nil / Err         │  │ • block scoping     │  │                         │  │
│  │ • Struct / Array    │  │                     │  │                         │  │
│  └─────────────────────┘  └─────────────────────┘  └─────────────────────────┘  │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘
```

#### 1. ENTRY POINTS

```cpp
evaluateDecl(ctx, decl)                         // Evaluate const variable
│
├── Check: decl->init exists
├── Check: circular dependency (m_evaluating.contains(decl))
├── Push: EvaluationGuard (mark as evaluating)
├── Push: Scope (make variable visible to itself)
├── result = evaluate(ctx, decl->init, decl->type)
├── Pop: Scope
└── Return: result

evaluate(ctx, expr, targetType)                 // Evaluate any expression
│
├── Check: expr != nullptr
├── Check: recursionDepth < MAX_RECURSION (1000)
├── Check: cache (m_evaluatedExprs.contains(expr))
│   └── If cached: return expr->constValue
│
├── Dispatch by expression kind:
│   ├── LiteralExpr        → evalLiteral()
│   ├── IdentifierExpr     → evalIdentifier()
│   ├── BinaryExpr         → evalBinary()
│   ├── UnaryExpr          → evalUnary()
│   ├── CallExpr           → evalCall()
│   ├── StructLiteralExpr  → evalStructLiteral()
│   ├── ArrayLiteralExpr   → evalArrayLiteral()
│   ├── FieldAccessExpr    → evalFieldAccess()
│   ├── NullCoalesceExpr   → evalNullCoalesce()
│   ├── IfExpr             → evalIfExpr()
│   └── RangeExpr          → evalRangeExpr()
│
├── Store on AST node:
│   ├── expr->isConst = true
│   ├── expr->constValue = result
│   ├── expr->resolvedType = getConstantType(result)
│   └── expr->valueState = Definite/Err
│
└── Return: result
```

#### 2. LITERAL EVALUATION

```cpp
evalLiteral(ctx, expr)
│
├── True/False        → ConstantValue(bool)
├── Int/Hex/Binary    → ConstantValue(std::stoll(value))
├── Float             → ConstantValue(std::stod(value))
├── String/RawString  → ConstantValue(value)
├── Char              → ConstantValue(value)
├── Nil               → ConstantValue::nil()
├── Err               → ConstantValue::err()
└── default           → ConstantValue::unknown()
```

#### 3. IDENTIFIER EVALUATION

```cpp
evalIdentifier(ctx, expr)
│
├── if name == "_" → return unknown() (discard placeholder)
│
├── decl = ctx.lookupValue(name)
│
├── // Variable ─────────────────────────────────────────────────────────────
│   if VarDecl:
│       if var->init && var->init->isConst → return constValue
│       if var->keyword == Const:
│           if m_evaluating.contains(var) → report cycle
│           return evaluate(ctx, var->init, var->type)
│       return unknown() (non-const variable)
│
├── // Function ─────────────────────────────────────────────────────────────
│   if FuncDecl:
│       if func->keyword != Const → return unknown()
│       return ConstantValue(func)  (function pointer)
│
├── // Enum Variant ─────────────────────────────────────────────────────────
│   if EnumVariantAST:
│       return ConstantValue(variant->value)
│
└── // Parameter ────────────────────────────────────────────────────────────
    if ParamAST:
        return unknown() (value bound during execution)
```

#### 4. BINARY EXPRESSION EVALUATION

```cpp
evalBinary(ctx, expr, targetType)
│
├── // Type Narrowing (if condition context) ──────────────────────────────
│   if ctx.stack.isIfConditionCtx():
│       info = detectNarrowingPattern(expr, ctx)
│       if info.hasNarrowing:
│           ctx.stack.setPendingNarrowing(info)
│           return unknown()  // condition is const
│
├── left = evaluate(ctx, expr->left, targetType)
│   if error/unknown → return
│
├── // Short-circuit Logic ─────────────────────────────────────────────────
│   if expr->op == And and left.isBool() and !left.asBool() → return false
│   if expr->op == Or  and left.isBool() and left.asBool()  → return true
│
├── right = evaluate(ctx, expr->right, targetType)
│   if error/unknown → return
│
└── return evalBinaryOp(ctx, op, left, right, expr, targetType)

evalBinaryOp(ctx, op, left, right, node, targetType)
│
├── // Arithmetic (numeric promotion) ──────────────────────────────────────
│   if op in (Add, Sub, Mul, Div, Mod, Pow):
│       if both numeric:
│           promote int → float if mixed
│           check overflow (Add/Sub/Mul/Div/Mod)
│           check division by zero (Div/Mod)
│           return result
│       else error
│
├── // String Concatenation ────────────────────────────────────────────────
│   if op == Add and left.isString() and right.isString():
│       return concat(left, right)
│
├── // Comparison ──────────────────────────────────────────────────────────
│   if op in (Eq, Ne, Lt, Gt, Le, Ge):
│       if kinds match:
│           return compare(left, right)
│       else error
│
├── // Logical ─────────────────────────────────────────────────────────────
│   if op in (And, Or):
│       if both bool: return left && right or left || right
│       else error
│
└── // Bitwise ─────────────────────────────────────────────────────────────
    if op in (BitAnd, BitOr, BitXor, Shl, Shr):
        if both int:
            check shift bounds (Shl/Shr)
            return bitwise operation
        else error
```

#### 5. UNARY EXPRESSION EVALUATION

```cpp
evalUnary(ctx, expr, targetType)
│
├── operand = evaluate(ctx, expr->operand, targetType)
│   if error/unknown → return
│
└── dispatch by op:
    ├── Neg:     if int → check INT64_MIN overflow, return -value
    │            if float → return -value
    │            else error
    ├── Not:     if bool → return !value
    │            else error
    └── BitNot:  if int → return ~value
                 else error
```

#### 6. CALL EXPRESSION (CONST FUNCTION)

```cpp
evalCall(ctx, expr)
│
├── func = resolveCalleeOrError(expr->callee, ctx)
│   if !func → return error
│
├── if func->keyword != Const → return unknown()
│
├── if func has generics and no args → return unknown()
│
├── // Evaluate arguments ──────────────────────────────────────────────────
│   for each arg in expr->args:
│       val = evaluate(ctx, arg)
│       if error/unknown → return
│
└── return executeFunction(ctx, func, args)

executeFunction(ctx, func, args)
│
├── // Recursion guard ─────────────────────────────────────────────────────
│   if recursionDepth >= MAX_RECURSION (1000) → error
│   recursionDepth++
│   DepthGuard (auto-decrement on exit)
│
├── // Setup context ────────────────────────────────────────────────────────
│   ConstFunctionContext:
│       ctx.stack.pushFunction(func, func->funcType->returnType)
│       ctx.pushScope()
│
├── // Bind arguments ──────────────────────────────────────────────────────
│   for each parameter:
│       param->type = getConstantType(args[index++])
│
├── // Execute body ────────────────────────────────────────────────────────
│   if func->body:
│       result = executeStmt(ctx, func->body)
│   else:
│       error: const function has no body
│
├── // Check return type ──────────────────────────────────────────────────
│   if returnType != void:
│       if result.isVoid() → error (non-void function returns nothing)
│   else:
│       if !result.isVoid() → error (void function returns value)
│
└── return result
```

#### 7. STATEMENT EXECUTION (FOR CONST FUNCTIONS)

```cpp
executeStmt(ctx, stmt)
│
└── dispatch by statement kind:
    ├── BlockStmt     → executeBlock()
    ├── ReturnStmt    → executeReturn()
    ├── IfStmt        → executeIf()
    ├── WhileStmt     → executeWhile()
    ├── ForStmt       → executeFor()
    ├── SwitchStmt    → executeSwitch()
    ├── ExprStmt      → executeExprStmt()
    └── DeclStmt      → executeDeclStmt()

executeBlock(ctx, block)
│
├── ctx.pushScope()
├── for each stmt in block->stmts:
│   ├── result = executeStmt(ctx, stmt)
│   ├── if error/unknown → break
│   └── if !result.isVoid() → break (return/break/continue)
├── ctx.popScope()
└── return result

executeIf(ctx, stmt)
│
├── ScopedIfCondition(ctx, stmt->elseBranch != nullptr)
│
├── cond = evaluate(ctx, stmt->condition)
│   if error/unknown → return
│   if !cond.isBool() → error
│
├── info = ctx.stack.getPendingNarrowing()
│   ctx.stack.clearPendingNarrowing()
│
├── if cond.asBool():
│   if stmt->thenBranch:
│       if info.hasNarrowing and !info.isEquality:
│           ScopedNarrowing(ctx, info.narrowings, false)
│       return executeStmt(ctx, stmt->thenBranch)
│
└── else:
    if stmt->elseBranch:
        if info.hasNarrowing and info.isEquality:
            ScopedNarrowing(ctx, info.narrowings, true)
        return executeStmt(ctx, stmt->elseBranch)

executeWhile(ctx, stmt)
│
├── iterations = 0
│
└── while true:
    ├── if ++iterations > MAX_ITERATIONS (10000) → return unknown()
    │
    ├── cond = evaluate(ctx, stmt->condition)
    │   if error/unknown → return
    │   if !cond.isBool() → error
    │   if !cond.asBool() → break (exit loop)
    │
    ├── result = executeStmt(ctx, stmt->body)
    │   if error/unknown → return
    │   if result.isVoid() → continue
    │
    └── return result (non-void)

executeFor(ctx, stmt)
│
├── if iterable is RangeExprAST:
│   ├── lo = evaluateAsInt(ctx, range->lo)
│   ├── hi = evaluateAsInt(ctx, range->hi)
│   ├── if lo or hi missing → return unknown()
│   │
│   ├── if inclusive: validate lo <= hi
│   ├── if exclusive: validate lo < hi
│   │   if invalid → error
│   │
│   ├── step = 1
│   │   if stmt->step:
│   │       step = evaluateAsInt(ctx, stmt->step)
│   │       if step missing or step <= 0 → error
│   │
│   ├── iterations = 0
│   │
│   └── for i = lo; condition; i += step:
│       ├── if ++iterations > MAX_ITERATIONS → return unknown()
│       ├── bind index variable (if present)
│       ├── result = executeStmt(ctx, stmt->body)
│       ├── if error/unknown → return
│       └── if !result.isVoid() → return result
│
└── else:
    ├── executeStmt(ctx, stmt->body)  // fallback
    └── return unknown()

executeSwitch(ctx, stmt)
│
├── subject = evaluate(ctx, stmt->subject)
│   if error → return
│
├── if subject.isUnknown():
│   // Can't evaluate - execute all cases for side effects
│   for each case: executeStmt(ctx, case->body)
│   if default: executeStmt(ctx, defaultBody)
│   return unknown()
│
├── // Match cases ─────────────────────────────────────────────────────────
│   for each case:
│       for each value:
│           if value is RangeExprAST:
│               lo = evaluateAsInt(range->lo)
│               hi = evaluateAsInt(range->hi)
│               if both valid and subject.isInt():
│                   matches = (subject >= lo && subject < hi) [exclusive]
│                           or (subject >= lo && subject <= hi) [inclusive]
│           else:
│               caseVal = evaluate(ctx, value)
│               if error/unknown → fallback
│               matches = compareEqual(subject, caseVal)
│
│           if matches:
│               return executeStmt(ctx, case->body)
│
└── // Default ─────────────────────────────────────────────────────────────
    if defaultBody:
        return executeStmt(ctx, defaultBody)
    return voidValue()

executeReturn(ctx, stmt)
│
├── if stmt->value:
│   result = evaluate(ctx, stmt->value)
│   if error/unknown → return
│   return result
│
└── return voidValue()

executeDeclStmt(ctx, stmt)
│
├── if VarDecl and const:
│   val = evaluate(ctx, var->init)
│   if error/unknown → return
│   var->init->isConst = true
│   var->init->constValue = val
│   ctx.insertValue(var)
│   return voidValue()
│
└── error: mutable locals not allowed in const functions
```

#### 8. STRUCT/ARRAY LITERAL EVALUATION

```cpp
evalStructLiteral(ctx, expr)
│
├── typeDecl = ctx.lookupType(expr->typeName)
│   if !typeDecl or not Struct → error
│
├── // Initialize with defaults ────────────────────────────────────────────
│   for each field:
│       if field->defaultVal:
│           val = evaluate(ctx, field->defaultVal, field->type)
│           if error/unknown → return
│           fields[field->name] = val
│
├── // Override with explicit initializers ────────────────────────────────
│   for each init in expr->inits:
│       if field not found → error
│       if const field assigned nil/err → error
│       val = evaluate(ctx, init->value, field->type)
│       if error/unknown → return
│       fields[init->name] = val
│
├── // Check missing required fields ──────────────────────────────────────
│   for each field:
│       if field not in fields:
│           if field->defaultVal → continue
│           if nullable/fallible → continue (optional)
│           → error: missing required field
│
└── return ConstantValue(Struct, fields)

evalArrayLiteral(ctx, expr)
│
├── // Evaluate all elements ──────────────────────────────────────────────
│   for each elem in expr->elements:
│       val = evaluate(ctx, elem)
│       if error/unknown → return
│
├── // Check type consistency ─────────────────────────────────────────────
│   firstType = elements[0].type
│   for each subsequent element:
│       if !typesEqual(elem.type, firstType) → error
│
└── return ConstantValue(Array, elements)
```

#### 9. FIELD ACCESS & NULL COALESCE

```cpp
evalFieldAccess(ctx, expr)
│
├── obj = evaluate(ctx, expr->object)
│   if error/unknown → return
│
├── if !obj.isStruct() → error
│
└── return obj.asStruct()[expr->fieldName] (or error if not found)

evalNullCoalesce(ctx, expr)
│
├── val = evaluate(ctx, expr->value)
│   if error → return
│
├── if val.isNil() or val.isErr():
│   return evaluate(ctx, expr->fallback)
│
└── if val.isUnknown(): return unknown()
    else: return val
```

#### 10. IF EXPRESSION & RANGE EVALUATION

```cpp
evalIfExpr(ctx, expr)
│
├── cond = evaluate(ctx, expr->condition)
│   if error/unknown → return
│   if !cond.isBool() → error
│
└── if cond.asBool():
        return evaluate(ctx, expr->thenBranch)
    else:
        return evaluate(ctx, expr->elseBranch)

evalRangeExpr(ctx, expr)
│
├── lo = evaluate(ctx, expr->lo)
│   hi = evaluate(ctx, expr->hi)
│   if error/unknown → return
│
├── if !lo.isInt() or !hi.isInt() → return unknown()
│
├── if inclusive: lo <= hi
│   if exclusive: lo < hi
│   if invalid → error
│
└── return unknown() (range value not representable)
```

#### 11. DEPENDENCY ANALYSIS

```cpp
buildDependencyGraph(ctx)
│
├── // Collect const declarations ──────────────────────────────────────────
│   for each module:
│       for each decl:
│           if VarDecl with Const → add to m_constDecls
│           if FuncDecl with Const → add to m_constDecls
│
├── // Build dependencies ──────────────────────────────────────────────────
│   for each decl in m_constDecls:
│       if VarDecl: collectDeps(ctx, var->init, deps)
│       if FuncDecl: collectDepsFromStmt(ctx, func->body, deps)
│       m_deps[decl] = deps
│
└── topologicalSort(ctx, m_deps)

topologicalSort(ctx, deps)
│
├── // Kahn's Algorithm ────────────────────────────────────────────────────'
│   Initialize inDegree for all decls
│   for each (decl, depsList):
│       for each dep in depsList:
│           graph[decl].push_back(dep)
│           inDegree[dep]++
│
│   queue = all decls with inDegree == 0
│   while queue not empty:
│       decl = queue.pop()
│       result.push_back(decl)
│       for each dep in graph[decl]:
│           inDegree[dep]--
│           if inDegree[dep] == 0: queue.push(dep)
│
└── // Cycle detection ─────────────────────────────────────────────────────
    if result.size() != deps.size():
        Find cycle participants (inDegree > 0)
        Report circular dependency error

collectDeps(ctx, expr, deps)
│
└── Walk expression tree:
    ├── IdentifierExpr:
    │   └── decl = ctx.lookupValue(name)
    │       ├── if VarDecl with Const → deps.push_back(var)
    │       └── if FuncDecl with Const → deps.push_back(func)
    │
    ├── BinaryExpr: collectDeps(left) + collectDeps(right)
    ├── UnaryExpr: collectDeps(operand)
    ├── CallExpr: collectDeps(callee) + collectDeps(all args)
    ├── FieldAccessExpr: collectDeps(object)
    ├── StructLiteralExpr: collectDeps(all field inits)
    └── ArrayLiteralExpr: collectDeps(all elements)

collectDepsFromStmt(ctx, stmt, deps)
│
└── Walk statement tree:
    ├── BlockStmt: collectDepsFromStmt(each stmt)
    ├── ExprStmt: collectDeps(expr)
    ├── ReturnStmt: collectDeps(value)
    ├── IfStmt: collectDeps(condition) + collectDepsFromStmt(then/else)
    ├── WhileStmt: collectDeps(condition) + collectDepsFromStmt(body)
    └── DeclStmt: if const VarDecl: collectDeps(init)
```

#### 12. RAII GUARDS

```swift
┌──────────────────────────────────────────────────────────────────────────────────┐
│                             RAII Guards Overview                                 │
├──────────────────────────────────────────────────────────────────────────────────┤
│                                                                                  │
│  ┌─────────────────────┐  ┌──────────────────────┐  ┌─────────────────────────┐  │
│  │   EvaluationGuard   │  │ ConstFunctionContext │  │       DepthGuard        │  │
│  ├─────────────────────┤  ├──────────────────────┤  ├─────────────────────────┤  │
│  │ Prevents circular   │  │ Sets up function     │  │ Prevents infinite       │  │
│  │ dependencies in     │  │ context for const    │  │ recursion in const      │  │
│  │ const declarations  │  │ function execution   │  │ function evaluation     │  │
│  └─────────────────────┘  └──────────────────────┘  └─────────────────────────┘  │
│                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────┘

EvaluationGuard
│
├── // Purpose ─────────────────────────────────────────────────────────────────
│   Prevents infinite recursion in circular dependencies between const
│   declarations. Tracks which declarations are currently being evaluated.
│
├── // Construction ────────────────────────────────────────────────────────────
│   EvaluationGuard(m_evaluating, decl)
│   ├── m_evaluating.insert(decl)
│   └── (decl is now marked as "currently evaluating")
│
├── // Destruction ─────────────────────────────────────────────────────────────
│   ~EvaluationGuard()
│   └── m_evaluating.erase(decl)
│
└── // Usage ──────────────────────────────────────────────────────────────────
    In evaluateDecl():
    │
    ├── if m_evaluating.contains(decl):
    │   └── Report circular dependency error
    │
    └── EvaluationGuard guard(m_evaluating, decl)
        // ... evaluate the declaration ...
        // Automatically removed when guard goes out of scope

    In evalIdentifier():
    │
    └── if m_evaluating.contains(var):
        └── Report circular dependency error

ConstFunctionContext
│
├── // Purpose ─────────────────────────────────────────────────────────────────
│   Sets up the semantic context for executing a const function. Pushes
│   a function context (for return validation) and a scope (for parameters).
│
├── // Construction ────────────────────────────────────────────────────────────
│   ConstFunctionContext(ctx, func)
│   ├── ctx.stack.pushFunction(func, func->funcType->returnType)
│   └── ctx.pushScope()
│
├── // Destruction ─────────────────────────────────────────────────────────────
│   ~ConstFunctionContext()
│   ├── ctx.popScope()
│   └── ctx.stack.pop()
│
└── // Usage ──────────────────────────────────────────────────────────────────
    In executeFunction():
    │
    └── ConstFunctionContext context(ctx, func)
        // ... bind parameters and execute body ...
        // Automatically cleaned up when context goes out of scope

DepthGuard
│
├── // Purpose ─────────────────────────────────────────────────────────────────
│   Prevents infinite recursion in const function calls by tracking the
│   call stack depth.
│
├── // Construction ────────────────────────────────────────────────────────────
│   DepthGuard depthGuard{m_recursionDepth}
│   └── m_recursionDepth++
│
├── // Destruction ─────────────────────────────────────────────────────────────
│   ~DepthGuard()
│   └── m_recursionDepth--
│
└── // Usage ──────────────────────────────────────────────────────────────────
    In executeFunction():
    │
    ├── if m_recursionDepth >= MAX_RECURSION (1000):
    │   └── Error: exceeded maximum recursion depth
    │
    └── DepthGuard depthGuard{m_recursionDepth}
        // ... execute function body ...
        // Automatically decremented when guard goes out of scope
```

#### 13. INTERNAL STATE

```swift
┌─────────────────────────────────────────────────────────────────────────────────┐
│                          Internal State Overview                               │
├─────────────────────────────────────────────────────────────────────────────────┤
│                                                                                 │
│  ┌──────────────────────────┐  ┌──────────────────────────┐                     │
│  │      m_constDecls        │  │         m_deps           │                     │
│  ├──────────────────────────┤  ├──────────────────────────┤                     │
│  │ All const declarations   │  │ Dependency graph:        │                     │
│  │ (VarDecl + FuncDecl)     │  │ decl → [dependencies]    │                     │
│  └──────────────────────────┘  └──────────────────────────┘                     │
│                                                                                 │
│  ┌──────────────────────────┐  ┌──────────────────────────┐                     │
│  │    m_evaluatedExprs      │  │      m_evaluating        │                     │
│  ├──────────────────────────┤  ├──────────────────────────┤                     │
│  │ Cache of evaluated       │  │ Set of declarations      │                     │
│  │ expressions (avoid       │  │ currently being          │                     │
│  │ re-evaluation)           │  │ evaluated (cycle         │                     │
│  │                          │  │ detection)              │                     │
│  └──────────────────────────┘  └──────────────────────────┘                     │
│                                                                                 │
│  ┌──────────────────────────┐                                                   │
│  │     m_recursionDepth     │                                                   │
│  ├──────────────────────────┤                                                   │
│  │ Current call stack depth │                                                   │
│  │ for const functions      │                                                   │
│  │ (MAX_RECURSION = 1000)   │                                                   │
│  └──────────────────────────┘                                                   │
│                                                                                 │
└─────────────────────────────────────────────────────────────────────────────────┘

m_constDecls : vector<DeclAST*>
│
├── // Purpose ─────────────────────────────────────────────────────────────────
│   Stores all const declarations (variables and functions) in the program.
│   Used by buildDependencyGraph() to build the dependency graph.
│
└── // Populated by ────────────────────────────────────────────────────────────
    buildDependencyGraph():
    │
    └── for each module:
        └── for each decl:
            ├── if VarDecl with Const → add to m_constDecls
            └── if FuncDecl with Const → add to m_constDecls

m_deps : unordered_map<DeclAST*, vector<DeclAST*>>
│
├── // Purpose ─────────────────────────────────────────────────────────────────
│   Dependency graph for const declarations. Maps each declaration to the
│   list of declarations it depends on.
│
└── // Populated by ────────────────────────────────────────────────────────────
    buildDependencyGraph():
    │
    └── for each decl in m_constDecls:
        ├── if VarDecl: collectDeps(ctx, var->init, deps)
        ├── if FuncDecl: collectDepsFromStmt(ctx, func->body, deps)
        └── m_deps[decl] = deps

m_evaluatedExprs : unordered_set<ExprAST*>
│
├── // Purpose ─────────────────────────────────────────────────────────────────
│   Cache of expressions that have already been evaluated. Prevents
│   re-evaluation of the same expression in different contexts.
│
└── // Populated by ────────────────────────────────────────────────────────────
    evaluate():
    │
    └── if result.isEvaluated() and !result.isError():
        └── m_evaluatedExprs.insert(expr)

m_evaluating : unordered_set<DeclAST*>
│
├── // Purpose ─────────────────────────────────────────────────────────────────
│   Tracks which declarations are currently being evaluated. Used for
│   circular dependency detection.
│
├── // Populated by ────────────────────────────────────────────────────────────
│   EvaluationGuard (construction): m_evaluating.insert(decl)
│   EvaluationGuard (destruction):  m_evaluating.erase(decl)
│
└── // Checked by ──────────────────────────────────────────────────────────────
    evaluateDecl():
    │
    └── if m_evaluating.contains(decl):
        └── Report circular dependency error

    evalIdentifier():
    │
    └── if m_evaluating.contains(var):
        └── Report circular dependency error

m_recursionDepth : size_t
│
├── // Purpose ─────────────────────────────────────────────────────────────────
│   Tracks the current call stack depth for const function evaluation.
│   Prevents infinite recursion.
│
├── // Modified by ─────────────────────────────────────────────────────────────
│   DepthGuard (construction): m_recursionDepth++
│   DepthGuard (destruction):  m_recursionDepth--
│
├── // Constants ───────────────────────────────────────────────────────────────
│   MAX_RECURSION = 1000  // Maximum allowed recursion depth
│   MAX_ITERATIONS = 10000 // Maximum loop iterations in const evaluation
│
└── // Checked by ──────────────────────────────────────────────────────────────
    evaluate():
    │
    └── if m_recursionDepth >= MAX_RECURSION:
        └── return ConstantValue::unknown()

    executeFunction():
    │
    └── if m_recursionDepth >= MAX_RECURSION:
        └── Error: exceeded maximum recursion depth
```

#### 14. CONSTANT VALUE TYPES

```swift
┌────────────────────────────────────────────────────────────────────────────────┐
│                         ConstantValue Structure                                │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                        ConstantValue                                    │   │
│  ├─────────────────────────────────────────────────────────────────────────┤   │
│  │  kind  : Kind enum (Bool, Int, Float, String, Char, Nil, Err, ...)      │   │
│  │  value : variant (bool, int64_t, double, InternedString, ...)           │   │
│  │  type  : TypeAST* (cached semantic type)                                │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌──────────────────────────────────────────────────────────────────────────┐  │
│  │                           Kind Enum                                      │  │
│  ├──────────────────────────────────────────────────────────────────────────┤  │
│  │  bool    │ Boolean (true/false)                                          │  │
│  │  Int     │ 64-bit integer                                                │  │
│  │  Float   │ 64-bit floating point                                         │  │
│  │  String  │ Interned string                                               │  │
│  │  Char    │ Single character                                              │  │
│  │  Nil     │ Null value (for nullable types)                               │  │
│  │  Err     │ Error value (for fallible types)                              │  │
│  │  Struct  │ Struct literal (field name → value)                           │  │
│  │  Array   │ Array literal (list of values)                                │  │
│  │  Func    │ Function pointer (for const functions)                        │  │
│  │  Void    │ No value (void functions)                                     │  │
│  │  Unknown │ Unknown value (could not evaluate)                            │  │
│  │  Error   │ Evaluation error                                              │  │
│  └──────────────────────────────────────────────────────────────────────────┘  │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```
```cpp
ConstantValue Methods:
│
├── // Constructors ────────────────────────────────────────────────────────────
│   ConstantValue(bool)              // Bool
│   ConstantValue(int64_t)           // Int
│   ConstantValue(double)            // Float
│   ConstantValue(InternedString)    // String or Char
│   ConstantValue(FuncDeclAST*)      // Func (function pointer)
│   ConstantValue(StructFields)      // Struct
│   ConstantValue(ArrayElements)     // Array
│
├── // Static Factory Methods ──────────────────────────────────────────────────
│   ConstantValue::nil()             // Nil
│   ConstantValue::err()             // Err
│   ConstantValue::voidValue()       // Void
│   ConstantValue::unknown()         // Unknown
│   ConstantValue::error()           // Error
│
└── // Predicates ──────────────────────────────────────────────────────────────
    bool isBool() const
    bool isInt() const
    bool isFloat() const
    bool isString() const
    bool isChar() const
    bool isNil() const
    bool isErr() const
    bool isStruct() const
    bool isArray() const
    bool isFunc() const
    bool isVoid() const
    bool isUnknown() const
    bool isError() const
    bool isEvaluated() const  // !isUnknown() && !isError()
```

#### 15. EVALUATION CATEGORIES

```swift
┌────────────────────────────────────────────────────────────────────────────────┐
│                      Evaluation Categories Summary                             │
├────────────────────────────────────────────────────────────────────────────────┤
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                    Basic Literal Evaluation                             │   │
│  ├─────────────────────────────────────────────────────────────────────────┤   │
│  │  Input          │ Output          │ Notes                               │   │
│  │  true/false     │ Bool            │ Direct conversion                   │   │
│  │  42, 0xFF, 0b10 │ Int             │ std::stoll with base detection      │   │
│  │  3.14           │ Float           │ std::stod                           │   │
│  │  "hello"        │ String          │ Interned string                     │   │
│  │  'a'            │ Char            │ Single character                    │   │
│  │  nil            │ Nil             │ Null value                          │   │
│  │  err            │ Err             │ Error value                         │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                    Identifier Resolution                                │   │
│  ├─────────────────────────────────────────────────────────────────────────┤   │
│  │  Scenario                     │ Result                                  │   │
│  │  const X = 42                 │ Returns 42                              │   │
│  │  let X = 42                   │ Unknown (non-const)                     │   │
│  │  X (currently evaluating)     │ Error (circular dependency)             │   │
│  │  X (const function)           │ Func pointer                            │   │
│  │  _ (discard)                  │ Unknown                                 │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                    Binary Operations                                    │   │
│  ├─────────────────────────────────────────────────────────────────────────┤   │
│  │  Category         │ Supported Types           │ Notes                   │   │
│  │  Arithmetic       │ Int, Float                │ Overflow checks         │   │
│  │  String Concat    │ String + String           │ Interned result         │   │
│  │  Comparison       │ All comparable types      │ Type mismatch error     │   │
│  │  Logical          │ Bool                      │ Short-circuit           │   │
│  │  Bitwise          │ Int                       │ Shift bounds check      │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                    Unary Operations                                     │   │
│  ├─────────────────────────────────────────────────────────────────────────┤   │
│  │  Operation        │ Supported Types         │ Notes                     │   │
│  │  - (negate)       │ Int, Float              │ INT64_MIN overflow check  │   │
│  │  not              │ Bool                    │ Logical negation          │   │
│  │  ~ (bitwise not)  │ Int                     │ Bitwise complement        │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                    Control Flow                                         │   │
│  ├─────────────────────────────────────────────────────────────────────────┤   │
│  │  Construct        │ Support      │ Notes                                │   │
│  │  if/else          │ Full         │ Type narrowing, branch selection     │   │
│  │  while loops      │ Full         │ MAX_ITERATIONS limit (10000)         │   │
│  │  for loops        │ Range only   │ Index variable binding               │   │
│  │  switch/case      │ Full         │ Range cases, exhaustiveness          │   │
│  │  return           │ Full         │ Type checking, early exit            │   │
│  │  blocks           │ Full         │ Scoping, local declarations          │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                    Composite Types                                      │   │
│  ├─────────────────────────────────────────────────────────────────────────┤   │
│  │  Type             │ Support      │ Notes                                │   │
│  │  Struct literals  │ Full         │ Default values, field validation     │   │
│  │  Array literals   │ Full         │ Type consistency check               │   │
│  │  Field access     │ Full         │ Struct field lookup                  │   │
│  │  Null coalesce    │ Full         │ nil/err fallback                     │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
│  ┌─────────────────────────────────────────────────────────────────────────┐   │
│  │                    Const Functions                                      │   │
│  ├─────────────────────────────────────────────────────────────────────────┤   │
│  │  Feature          │ Support      │ Notes                                │   │
│  │  Function calls   │ Full         │ Recursion limit (1000)               │   │
│  │  Parameters       │ Full         │ Type binding                         │   │
│  │  Return values    │ Full         │ Type checking                        │   │
│  │  Local variables  │ Const only   │ Mutable vars not allowed             │   │
│  │  Generic functions│ Limited      │ Must provide explicit args           │   │
│  └─────────────────────────────────────────────────────────────────────────┘   │
│                                                                                │
└────────────────────────────────────────────────────────────────────────────────┘
```

## Precedence and Rules

### Type Narrowing Rules

| Pattern    | Effect                              | isEquality |
| ---------- | ----------------------------------- | ---------- |
| `x != nil` | `x` → `inner type` in then branch   | `false`    |
| `x == nil` | `x` → `inner type` in rest of block | `true`     |
| `x != err` | `x` → `inner type` in then branch   | `false`    |
| `x == err` | `x` → `inner type` in rest of block | `true`     |
| `await x`  | `Future<T>` → `T` in rest of block  | `true`     |
| `join x`   | `Thread<T>` → `T` in rest of block  | `true`     |

### Downward Flow Rule

Borrowed types (`&T`, `[_]T`) cannot appear in:
1. Struct fields
2. Array/Slice elements
3. Function returns
4. Closure captures

### Const Evaluation Rules

| Construct      | Const Evaluable? | Notes                                           |
| -------------- | ---------------- | ----------------------------------------------- |
| Literals       | ✅                | True, False, Int, Float, String, Char, Nil, Err |
| Identifier     | ✅                | Only if references const declaration            |
| Binary         | ✅                | Arithmetic, comparison, logical, bitwise        |
| Unary          | ✅                | Neg, Not, BitNot                                |
| Call           | ✅                | Only const functions                            |
| Struct literal | ✅                | If all fields are const                         |
| Array literal  | ✅                | If all elements are const                       |
| Field access   | ✅                | If object is const                              |
| Null coalesce  | ✅                | If value is const                               |
| If expression  | ✅                | If condition and branches are const             |
| Range          | ✅                | If bounds are const                             |
| Intrinsic      | ⚠️                | Only deterministic intrinsics                   |

### Capture Rules

| Rule               | Description                                |
| ------------------ | ------------------------------------------ |
| No borrowed types  | Cannot capture `&T` or `[_]T`              |
| No linear types    | Cannot capture `Future<T>` or `Thread<T>`  |
| Module members     | Never captured (global)                    |
| Local variables    | Captured if referenced from inner function |
| Generic parameters | Never captured                             |

## Helper Functions

### Type Predicates (`SemaCompare.hpp`)

```cpp
bool typesEqual(a, b);                    // Deep structural equality
bool isAssignable(target, source, ctx);    // Assignability check
bool isNullableType(type);                // T? or T?!
bool isFallibleType(type);                // T! or T?!
bool isReferenceType(type);               // &T
bool isPointerType(type);                 // *T
bool isPrimitiveType(type);               // Any built-in type
bool isIntegerType(type);                 // int, uint, int8, etc.
bool isFloatType(type);                   // float, double, decimal
bool isNumericType(type);                 // Integer or float
bool isBoolType(type);                    // bool
bool isStringType(type);                  // string
bool isCharType(type);                    // char
bool isStructType(type, ctx);             // Named type → StructDecl
bool isEnumType(type, ctx);               // Named type → EnumDecl
bool isTraitType(type, ctx);              // Named type → TraitDecl
bool isBorrowedType(type);                // &T or [_]T
```

### Type Resolution (`SemaResolve.hpp`)

```cpp
TypeAST* resolveType(type, ctx);                             // Main entry
TypeAST* resolvePrimitiveType(type, ctx);                    // Built-in types
TypeAST* resolveNamedType(type, ctx);                        // User-defined types
TypeAST* resolveModuleTypeAccess(type, ctx);                 // module:Type
TypeAST* resolveArrayType(type, ctx);                        // [N]T, [*]T, [_]T
TypeAST* resolveNullableType(type, ctx);                     // T?
TypeAST* resolveFallibleType(type, ctx);                     // T!
TypeAST* resolveCombinedType(type, ctx);                     // T?!
TypeAST* resolveRefType(type, ctx);                          // &T
TypeAST* resolvePtrType(type, ctx);                          // *T
TypeAST* resolveFuncType(type, ctx);                         // (T) -> R
```

### Semantic Validation (`SemaValidate.hpp`)

```cpp
bool validateConstType(type, name, kind, ctx);               // Must be definite
bool validateConstInitializer(hasInit, name, kind, ctx);    // Must have init
bool validateTraitImplementation(structDecl, traitDecl, ctx); // Field matching
bool validateAllTraitImplementations(structDecl, ctx);       // All traits
bool validateGenericArguments(args, params, useSite, ctx);   // Arity + constraints
bool validateBorrowedContext(type, ctx);                     // Downward Flow
bool validateForeignFunction(decl, attr, ctx);               // ABI + FFI
```

### Const Evaluation (`ConstEvaluator.hpp`)

```cpp
ConstantValue evaluateDecl(ctx, decl);                       // Evaluate const decl
ConstantValue evaluate(ctx, expr, targetType);              // Evaluate expression
bool isConstExpr(ctx, expr, targetType);                    // Check constness
std::optional<int64_t> evaluateAsInt(ctx, expr);           // Evaluate as int
std::optional<bool> evaluateAsBool(ctx, expr);             // Evaluate as bool
```

### Capture Analysis (`CaptureAnalysis.hpp`)

```cpp
void analyzeCaptures(AnonFuncExprAST* expr, ctx);           // Closure captures
void analyzeCaptures(FuncDeclAST* func, ctx);               // Nested function captures
void markClosureIfEscaping(ExprAST* expr, ctx);             // Escape analysis
```