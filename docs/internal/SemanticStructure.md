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

The semantic analysis phase is orchestrated by the `analyze()` function, which serves as the main entry point for the semantic analyzer. It operates in two distinct passes to ensure correct name resolution and type checking.

**Entry Point Overview**

The `analyze()` function takes a vector of parsed modules and a `SemaContext` (which holds all state including symbol tables, type cache, and diagnostics). It performs two passes: first registering all top-level names, then resolving types and checking bodies.

#### PHASE 1: Register ALL top-level names (No type resolution)

```cpp
analyze(modules, ctx)
│
├── for each module:
│   │
│   ├── ctx.enterModule(module)
│   │
│   └── registerTopLevelNames(module, ctx)
│       │
│       └── for each decl in module->decls:
│           │
│           └── registerDeclName(decl, ctx)
│               │
│               ├── ImportDecl   → registerImportName()
│               ├── VarDecl      → registerVarName()
│               ├── FuncDecl     → registerFuncName()
│               ├── StructDecl   → registerStructName()
│               │                    └── registerStructFieldNames()
│               ├── EnumDecl     → registerEnumName()
│               └── TraitDecl    → registerTraitName()
│
└── // After all modules registered, proceed to Phase 2
```

> [!IMPORTANT]
> Phase 1 ONLY registers top-level declarations. Local variables, parameters, and other scoped names are registered during Phase 2 when we actually resolve the bodies.

**Key Points:**
- Only top-level names are registered (functions, structs, enums, traits, imports)
- No type resolution occurs
- Local variables and parameters are not registered until Phase 2
- Struct field names are registered as part of `registerStructName()`

#### PHASE 2: Resolve ALL types, check bodies, AND evaluate consts

```cpp
analyze() ─── continues to Phase 2
│
├── for each module:
│   │
│   ├── ctx.enterModule(module)
│   │
│   └── resolveModuleDecls(module, ctx)
│       │
│       └── for each decl in module->decls:
│           │
│           └── resolveDecl(decl, ctx)
│               │
│               ├── if NOT at module level:
│               │   ├── VarDecl/FuncDecl  → ctx.insertValue()
│               │   └── Struct/Enum/Trait → ctx.insertType()
│               │
│               └── dispatch by kind:
│                   │
│                   ├── ImportDecl   → resolveImportDecl()
│                   ├── VarDecl      → resolveVarDecl()
│                   │                   └── ConstEvaluator::evaluateDecl()
│                   ├── FuncDecl     → resolveFuncDecl()
│                   │                   ├── registerParamName()
│                   │                   ├── registerGenericParamName()
│                   │                   └── resolveStmt(body)
│                   ├── StructDecl   → resolveStructDecl()
│                   │                   ├── resolveTraitRefs()
│                   │                   └── resolveStructFields()
│                   ├── EnumDecl     → resolveEnumDecl()
│                   └── TraitDecl    → resolveTraitDecl()
│
└── // All declarations resolved
```

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
resolveDecl(decl, ctx)
│
├── // PHASE 2 REGISTRATION (Nested declarations only)
│   └── if NOT at module level:
│       ├── VarDecl/FuncDecl  → ctx.insertValue(decl->as<ValueDeclAST>())
│       └── Struct/Enum/Trait → ctx.insertType(decl->as<TypeDeclAST>())
│
└── // DISPATCH BY KIND
    │
    ├── ImportDecl
    │   └── resolveImportDecl(decl, ctx)
    │       ├── Validate imported module exists
    │       └── No further resolution needed (Phase 1 already registered)
    │
    ├── VarDecl
    │   └── resolveVarDecl(decl, ctx)
    │       ├── resolveType(decl->type) → validate type exists
    │       ├── if const: validateConstType() & validateConstInitializer()
    │       ├── if init: resolveExprWithTarget(init, type) → type check
    │       ├── if const: ConstEvaluator::evaluateDecl() → evaluate at compile time
    │       └── if let: checkLetSelfReference(init, name) → prevent self-reference
    │
    ├── FuncDecl
    │   └── resolveFuncDecl(decl, ctx)
    │       ├── resolveFuncType(decl->funcType) → validate function signature
    │       ├── registerGenericParamName() for each generic parameter
    │       ├── for each param: resolveParam(param, ctx) → resolve parameter type
    │       │   └── registerParamName(param, ctx) → register in current scope
    │       ├── if body: resolveBlock(body, ctx) → analyze function body
    │       ├── analyzeCaptures(func, ctx) → detect captured variables
    │       └── if '@[foreign]': validateForeignFunction() → ABI & FFI checks
    │
    ├── StructDecl
    │   └── resolveStructDecl(decl, ctx)
    │       ├── resolveTraitRefs(traitRefs, ctx) → resolve each trait reference
    │       ├── registerStructFieldNames(decl, ctx) → Phase 1 already did this
    │       ├── ScopedTypeDefinition(ctx, decl) → track for self-reference checks
    │       └── resolveStructFields(decl, ctx) → resolve each field's type
    │           ├── for each field:
    │           │   ├── resolveType(field->type) → validate type exists
    │           │   ├── validateBorrowedContext(field->type, ctx) → Downward Flow
    │           │   └── if const: validateConstType() & validateConstInitializer()
    │           └── validateAllTraitImplementations(structDecl, ctx)
    │
    ├── EnumDecl
    │   └── resolveEnumDecl(decl, ctx)
    │       ├── resolveType(decl->backingType) → validate backing type exists
    │       ├── check backing type is integer primitive
    │       └── for each variant:
    │           ├── check value uniqueness → no duplicate values
    │           └── ensure value fits in backing type
    │
    └── TraitDecl
        └── resolveTraitDecl(decl, ctx)
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

### Function Declaration Resolution (`resolveFuncDecl`)

```cpp
resolveFuncDecl(decl, ctx)
│
├── // 1. RESOLVE FUNCTION TYPE
│   └── resolveFuncType(decl->funcType)
│       ├── for each param: resolveType(param->type)
│       └── if returnType: resolveType(returnType)
│
├── // 2. REGISTER GENERIC PARAMETERS
│   └── for each genericParam: ctx.registerGenericParam(genericParam)
│
├── // 3. RESOLVE PARAMETERS
│   └── for each param in funcType->params:
│       ├── resolveType(param->type)
│       └── ctx.registerParamName(param)
│
├── // 4. RESOLVE BODY (if present)
│   └── if decl->body:
│       ├── ScopedSemanticContext(ctx, FuncBody)
│       ├── SymbolScope(ctx)
│       └── resolveBlock(decl->body, ctx)
│           ├── tracks return type via ctx.stack.currentReturnType()
│           └── validates return statements match function return type
│
├── // 5. CAPTURE ANALYSIS
│   └── analyzeCaptures(decl, ctx)
│       ├── walk AST for identifier references from outer scopes
│       └── validate no borrowed types captured
│
└── // 6. FOREIGN FUNCTION VALIDATION
    └── if '@[foreign]' attribute:
        └── validateForeignFunction(decl, attr, ctx)
            ├── check symbol exists in FFI table
            ├── check parameter types match C ABI
            └── check return type matches C ABI
```

---

### Statement Resolution (`resolveStmt`)

The `resolveStmt()` function is the main entry point for resolving and validating statements during Phase 2. It dispatches to specific statement resolvers and manages control flow analysis.

**Purpose:**
- Validates statement semantics (e.g., `break` inside loops, `return` inside functions)
- Performs type checking on expressions within statements
- Manages control flow analysis (detects unreachable code, return paths)
- Handles type narrowing for `if` statements and concurrency statements

```cpp
resolveStmt(stmt, ctx)
│
├── // DISPATCH BY KIND
│
    ├── BlockStmt
    │   └── resolveBlock(stmt, ctx)
    │       ├── if pending inverse narrowing: apply it
    │       │   ├── ctx.stack.pushNarrowingLevel(true)
    │       │   └── ctx.stack.narrowVariable() for each narrowed variable
    │       ├── SymbolScope(ctx) → create new lexical scope
    │       ├── ScopedSemanticContext(ctx, Block) → push block context
    │       ├── for each stmt:
    │       │   ├── if transfers: warning (unreachable code)
    │       │   └── transfers = resolveStmt(stmt, ctx)
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
    │       ├── CONST EVALUATION: evaluate condition at compile time
    │       │   ├── if const true: only resolve then branch
    │       │   └── if const false: only resolve else branch
    │       ├── info = ctx.stack.getPendingNarrowing() → capture narrowing info
    │       ├── THEN BRANCH:
    │       │   ├── SymbolScope(ctx) → new scope
    │       │   ├── if hasNarrowing && !info.isEquality:
    │       │   │   └── ScopedNarrowing(ctx, info.narrowings, false)
    │       │   └── thenReturns = resolveStmt(thenBranch, ctx)
    │       ├── ELSE BRANCH:
    │       │   ├── SymbolScope(ctx) → new scope
    │       │   ├── if hasNarrowing && info.isEquality:
    │       │   │   └── ScopedNarrowing(ctx, info.narrowings, true)
    │       │   └── elseReturns = resolveStmt(elseBranch, ctx)
    │       ├── if no else and thenReturns:
    │       │   └── ctx.stack.setPendingInverseNarrowing(info)
    │       └── return thenReturns && elseReturns
    │
    ├── SwitchStmt
    │   └── resolveSwitchStmt(stmt, ctx)
    │       ├── resolveExprWithTarget(subject) → type check subject
    │       ├── isValidSwitchType(subjectType) → integer, enum, bool, char, string
    │       ├── ScopedSemanticContext(ctx, SwitchBody) → push switch context
    │       ├── CONST EVALUATION: evaluate subject at compile time
    │       ├── for each case:
    │       │   ├── for each value: resolveExprWithTarget(value, subjectType)
    │       │   ├── isSwitchCaseCompatible(value, subjectType) → validate
    │       │   ├── duplicate case detection:
    │       │   │   ├── literal values → check uniqueness
    │       │   │   ├── enum variants → check uniqueness
    │       │   │   └── ranges → check overlap
    │       │   └── resolveBlock(case->body) → resolve case body
    │       ├── if isEnumType(subjectType):
    │       │   └── switch_helpers::checkExhaustiveness() → ensure all variants covered
    │       ├── if defaultBody: resolveBlock(defaultBody) → resolve default
    │       └── return allCasesReturn && (defaultBody || !isEnumType)
    │
    ├── ForStmt
    │   └── resolveForStmt(stmt, ctx)
    │       ├── ScopedSemanticContext(ctx, LoopBody) → push loop context
    │       ├── SymbolScope(ctx) → new scope for loop variables
    │       ├── if range loop (no valueVar):
    │       │   ├── resolveRangeExpr(range) → validate range bounds
    │       │   ├── CONST EVALUATION: validate range at compile time
    │       │   │   ├── evaluateAsInt(lo) & evaluateAsInt(hi)
    │       │   │   ├── if inclusive: lo <= hi
    │       │   │   └── if exclusive: lo < hi
    │       │   ├── if indexVar: ctx.insertValue(indexVar) → register index
    │       │   └── if step: resolveExprWithTarget(step, intType)
    │       ├── if collection loop (has valueVar):
    │       │   ├── resolveExpr(iterable) → resolve collection
    │       │   ├── if indexVar: ctx.insertValue(indexVar) → register index
    │       │   ├── if valueVar: ctx.insertValue(valueVar) → register value
    │       │   └── validate value type matches iterable element type
    │       ├── resolveBlock(body) → resolve loop body
    │       └── return false (loops dont guarantee return unless break/return)
    │
    ├── WhileStmt
    │   └── resolveWhileStmt(stmt, ctx)
    │       ├── ScopedSemanticContext(ctx, LoopBody) → push loop context
    │       ├── resolveExprWithTarget(condition, boolType) → type check
    │       ├── CONST EVALUATION: check if condition is compile-time constant
    │       │   ├── if const false → warning (body unreachable)
    │       │   └── if const true → warning (infinite loop)
    │       ├── resolveBlock(body) → resolve loop body
    │       └── return false (loops dont guarantee return)
    │
    ├── ReturnStmt
    │   └── resolveReturnStmt(stmt, ctx)
    │       ├── if !ctx.stack.insideFunction() → error (return outside function)
    │       ├── expectedType = ctx.stack.currentReturnType()
    │       ├── if stmt->value:
    │       │   ├── if !expectedType → error (return value in void function)
    │       │   ├── resolveExprWithTarget(value, expectedType) → type check
    │       │   ├── markClosureIfEscaping(value, ctx) → detect closure return
    │       │   ├── if valueState == Err && !isFallibleType(expectedType) → error
    │       │   └── if valueState == Nil && !isNullableType(expectedType) → error
    │       ├── else:
    │       │   └── if expectedType → error (missing return value)
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
    │       └── return false (expression statements dont transfer control)
    │
    ├── DeclStmt
    │   └── resolveDeclStmt(stmt, ctx)
    │       └── resolveDecl(decl, ctx) → recursive call to resolve declaration
    │           └── return false (declarations dont transfer control)
    │
    ├── AsyncStmt
    │   └── resolveAsyncStmt(stmt, ctx)
    │       ├── if !ctx.stack.insideFunction() → error
    │       ├── resolveType(binding->type) → must be FutureTypeAST
    │       ├── ctx.insertValue(binding) → register binding
    │       ├── resolveExprWithTarget(call, innerType) → call must return inner type
    │       └── ctx.addPendingAsync(name, call, loc) → register for await
    │
    ├── AwaitStmt
    │   └── resolveAwaitStmt(stmt, ctx)
    │       ├── if !ctx.stack.insideFunction() → error
    │       ├── for each target:
    │       │   ├── if hasPendingAsync(targetName):
    │       │   │   ├── unwrap FutureTypeAST → get innerType
    │       │   │   ├── ctx.stack.narrowVariable(targetName, innerType)
    │       │   │   └── ctx.resolveAsync(targetName) → remove from pending
    │       │   └── else: error (not a pending async)
    │       └── return false
    │
    ├── SpawnStmt
    │   └── resolveSpawnStmt(stmt, ctx)
    │       ├── if !ctx.stack.insideFunction() → error
    │       ├── if binding:
    │       │   ├── resolveType(binding->type) → must be ThreadTypeAST
    │       │   ├── ctx.insertValue(binding) → register binding
    │       │   ├── resolveExprWithTarget(call, innerType)
    │       │   └── ctx.addPendingSpawn(name, call, loc) → register for join
    │       └── else (discard pattern): resolveExpr(call) → fire-and-forget
    │
    └── JoinStmt
        └── resolveJoinStmt(stmt, ctx)
            ├── if !ctx.stack.insideFunction() → error
            ├── for each target:
            │   ├── if hasPendingSpawn(targetName):
            │   │   ├── unwrap ThreadTypeAST → get innerType
            │   │   ├── ctx.stack.narrowVariable(targetName, innerType)
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

### Expression Resolution (`resolveExprWithTarget`)

The `resolveExprWithTarget()` function is the main entry point for resolving and type-checking expressions during Phase 2.

**Purpose:**
- Resolves the type of every expression in the AST
- Validates expressions against expected types (if provided)
- Performs operator validation and type promotion
- Detects type narrowing patterns in conditions
- Stores resolved type, value state, l-value status, and const status

```cpp
resolveExprWithTarget(expr, targetType, ctx)
│
├── // DISPATCH BY KIND
│
    ├── LiteralExpr
    │   └── resolveLiteralExpr(expr, targetType, ctx)
    │       ├── True/False → boolType, ValueState::Definite, isLValue=false
    │       ├── Int/Hex/Binary → intType (or target if specified)
    │       ├── Float → floatType (or target if specified)
    │       ├── String → stringType, ValueState::Definite
    │       ├── Char → charType, ValueState::Definite
    │       ├── Nil → targetType if nullable, else UnknownType
    │       └── Err → targetType if fallible, else UnknownType
    │
    ├── IdentifierExpr
    │   └── resolveIdentifierExpr(expr, targetType, ctx)
    │       ├── if name == "_": discard placeholder (UnknownType)
    │       ├── if isGenericParam(name): error (type param used as value)
    │       ├── lookupValue(name) → ValueDeclAST
    │       │   └── if not found: error (undefined value)
    │       ├── if pending future: error (await/join first)
    │       ├── if isCaptured && isBorrowedType(declType): error
    │       │   └── closures cannot capture &T or [_]T
    │       ├── if genericArgs: validateGenericArguments()
    │       ├── set isLValue, isConst from declaration keyword
    │       │   ├── let → isLValue=true, isConst=false
    │       │   └── const → isLValue=false, isConst=true
    │       └── apply type narrowing from ContextStack
    │           └── if getNarrowedType(name) → use narrowed type
    │
    ├── ArrayLiteralExpr
    │   └── resolveArrayLiteralExpr(expr, targetType, ctx)
    │       ├── if empty: use targetType element type or UnknownType
    │       ├── resolve first element → firstType
    │       ├── for each element: resolveExprWithTarget(elem, firstType)
    │       │   └── if type mismatch: error (array elements must have same type)
    │       └── getArrayType(kind, size, elementType) → cached array type
    │
    ├── StructLiteralExpr
    │   └── resolveStructLiteralExpr(expr, targetType, ctx)
    │       ├── lookupType(typeName) → StructDeclAST
    │       ├── validateGenericArguments() → validate generic args
    │       ├── for each field init:
    │       │   ├── lookup field in struct → FieldDeclAST
    │       │   ├── if const field: cannot assign nil/err
    │       │   ├── resolveExprWithTarget(value, field->type)
    │       │   └── if function field: must be function value
    │       └── check missing required fields
    │           ├── if field has default or nullable/fallible → optional
    │           └── else: error (missing required field)
    │
    ├── BinaryExpr
    │   └── resolveBinaryExpr(expr, targetType, ctx)
    │       ├── resolve left and right operands
    │       ├── if in if condition: detectNarrowingPattern()
    │       │   └── if found: return boolType (narrowing info stored)
    │       ├── Arithmetic (Add, Sub, Mul, Div, Pow, Mod):
    │       │   ├── reject nullable/fallible operands (must narrow first)
    │       │   ├── if numeric: promote int → float if mixed
    │       │   └── else: error (arithmetic requires numeric operands)
    │       ├── Comparison (Eq, Ne, Lt, Gt, Le, Ge):
    │       │   ├── if numeric: allow mixed types (promotion)
    │       │   ├── if non-numeric: must be same type
    │       │   └── return boolType
    │       ├── Logical (And, Or):
    │       │   ├── reject nullable/fallible operands (must narrow first)
    │       │   ├── require bool operands
    │       │   └── return boolType
    │       └── Bitwise (BitAnd, BitOr, BitXor, Shl, Shr):
    │           ├── reject nullable/fallible operands (must narrow first)
    │           ├── require integer operands
    │           └── promote to larger integer type
    │
    ├── UnaryExpr
    │   └── resolveUnaryExpr(expr, targetType, ctx)
    │       ├── resolve operand
    │       ├── Neg: require numeric, reject nullable/fallible
    │       ├── Not: require bool, reject nullable/fallible
    │       └── BitNot: require integer, reject nullable/fallible
    │
    ├── CallExpr
    │   └── resolveCallExpr(expr, targetType, ctx)
    │       ├── resolveExpr(callee) → must be FuncTypeAST
    │       ├── resolveCalleeOrError() → FuncDeclAST
    │       ├── validateGenericArguments() if generic function
    │       ├── check arg count with variadic support:
    │       │   ├── requiredArgs = params before variadic
    │       │   ├── if hasVariadic: args >= requiredArgs
    │       │   └── if no variadic: args == param count
    │       ├── for each arg: resolveExprWithTarget(arg, expectedType)
    │       │   ├── if arg is Err → must be fallible expected type
    │       │   └── if arg is Nil → must be nullable expected type
    │       └── return funcType->returnType
    │
    ├── IntrinsicCallExpr
    │   └── resolveIntrinsicCallExpr(expr, targetType, ctx)
    │       ├── validateIntrinsicCall(expr, ctx)
    │       │   ├── validateScopeExit() registers callback
    │       │   └── validate argument count and types
    │       ├── if void: return nullptr (statement-only intrinsic)
    │       ├── getIntrinsicReturnType(expr, targetType, ctx)
    │       └── set IntrinsicRegistry::llvmID for code generation
    │
    ├── IndexExpr
    │   └── resolveIndexExpr(expr, targetType, ctx)
    │       ├── resolveExpr(target) → must be ArrayTypeAST
    │       ├── reject nullable/fallible target (must narrow first)
    │       ├── resolveExprWithTarget(index, intType) → index must be integer
    │       ├── isLValue = target->isLValue (propagate l-value)
    │       └── return arrayType->element
    │
    ├── SliceExpr
    │   └── resolveSliceExpr(expr, targetType, ctx)
    │       ├── resolveExpr(target) → must be ArrayTypeAST
    │       ├── reject nullable/fallible target (must narrow first)
    │       ├── resolve start/end against intType (optional)
    │       ├── result is always [_]T (slice)
    │       └── isLValue=false (slices are never l-values)
    │
    ├── FieldAccessExpr
    │   └── resolveFieldAccessExpr(expr, targetType, ctx)
    │       ├── resolveExpr(object) → NamedTypeAST
    │       ├── reject nullable/fallible object (must narrow first)
    │       ├── if generic type: isFieldAccessibleOnGenericType()
    │       ├── lookupType(name) → StructDeclAST or EnumDeclAST
    │       ├── find field in struct fields or enum variants
    │       └── return field->type
    │
    ├── ModuleAccessExpr
    │   └── resolveModuleAccessExpr(expr, targetType, ctx)
    │       ├── lookupValueByAlias(moduleName, memberName)
    │       ├── check isValueExported() → must have @[export]
    │       ├── if generic: validateGenericArguments()
    │       └── return decl->type
    │
    ├── NullCoalesceExpr
    │   └── resolveNullCoalesceExpr(expr, targetType, ctx)
    │       ├── resolveExpr(value) → must be nullable or fallible
    │       ├── unwrapNullable/Fallible() → get inner type
    │       ├── resolveExprWithTarget(fallback, innerType)
    │       └── return innerType (or fallback type)
    │
    ├── AssignExpr
    │   └── resolveAssignExpr(expr, targetType, ctx)
    │       ├── resolveExpr(lhs) → get lhsType
    │       ├── check lhs->isLValue → must be assignable
    │       ├── check !lhs->isConst → cannot assign to const
    │       ├── if compound assignment:
    │       │   ├── reject nullable/fallible LHS (must narrow first)
    │       │   └── validate operator type (numeric or integer)
    │       └── return lhsType
    │
    ├── PipelineExpr
    │   └── resolvePipelineExpr(expr, targetType, ctx)
    │       ├── resolveExpr(seed) → get input type
    │       └── for each step: resolvePipelineStep(step, currentType)
    │           ├── resolveExpr(callable) → FuncTypeAST
    │           ├── check first param matches inputType
    │           └── currentType = funcType->returnType
    │
    ├── ComposeExpr
    │   └── resolveComposeExpr(expr, targetType, ctx)
    │       ├── resolveComposeOperand(left) → FuncTypeAST
    │       └── for each operand:
    │           ├── resolveComposeOperand(operand) → FuncTypeAST
    │           ├── check prev output → next input assignable
    │           └── currentFunc = nextFunc
    │
    ├── AnonFuncExpr
    │   └── resolveAnonFuncExpr(expr, targetType, ctx)
    │       ├── resolveFuncType(funcType) → validate signature
    │       ├── pushScope() → new scope for parameters
    │       ├── for each param: resolveParam(param, ctx)
    │       │   └── registerParamName(param, ctx)
    │       ├── ctx.stack.pushAnonFunction(expr, returnType)
    │       ├── resolveBlock(body) → analyze body
    │       ├── ctx.stack.pop() → pop function context
    │       ├── analyzeCaptures(expr, ctx) → detect captures
    │       │   ├── walk AST for identifier references
    │       │   ├── check if variable is from outer scope
    │       │   └── validate borrowed types cannot be captured
    │       └── return funcType
    │
    ├── IfExpr
    │   └── resolveIfExpr(expr, targetType, ctx)
    │       ├── resolveExprWithTarget(condition, boolType)
    │       ├── resolveExpr(thenBranch) → thenType
    │       ├── resolveExpr(elseBranch) → elseType
    │       ├── check isAssignable(thenType, elseType)
    │       └── return thenType
    │
    └── RangeExpr
        └── resolveRangeExpr(expr, targetType, ctx)
            ├── resolveExprWithTarget(lo, intType)
            ├── resolveExprWithTarget(hi, intType)
            ├── if inclusive: lo <= hi
            └── return loType (or intType)
│
├── // VALIDATE AGAINST TARGET TYPE
│   └── if targetType && !isAssignable(targetType, result, ctx):
│       └── error (type mismatch)
│
└── // STORE RESULT ON AST NODE
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

```
resolveType(type, ctx)
│
├── // DISPATCH BY KIND
│
    ├── PrimitiveTypeAST
    │   └── resolvePrimitiveType(type, ctx)
    │       ├── Primitive types are built-in and always valid
    │       └── return type (no validation needed)
    │
    ├── NamedTypeAST
    │   └── resolveNamedType(type, ctx)
    │       ├── Step 1: Check if this is a generic parameter
    │       │   └── if ctx.isGenericParam(name): return type (generic param)
    │       ├── Step 2: Look up as concrete type
    │       │   └── ctx.lookupType(name) → TypeDeclAST
    │       │       ├── StructDeclAST  → struct type
    │       │       ├── EnumDeclAST    → enum type
    │       │       └── TraitDeclAST   → trait type
    │       ├── Step 3: If not found → error (undefined type)
    │       ├── Step 4: Validate generic arguments if present
    │       │   ├── Check arity matches declaration parameters
    │       │   ├── Resolve each generic argument type
    │       │   └── validateGenericArguments(args, params, useSite)
    │       └── Step 5: Return cached NamedTypeAST
    │
    ├── ModuleTypeAccessAST
    │   └── resolveModuleTypeAccess(type, ctx)
    │       ├── Step 1: Look up the module alias
    │       │   └── ctx.lookupImport(moduleName) → ModuleAST
    │       ├── Step 2: Look up the type in the module's table
    │       │   └── ctx.lookupModuleTypeMember(module, memberName)
    │       ├── Step 3: Check if the type is exported
    │       │   └── if !ctx.isTypeExported(decl): error (private member)
    │       ├── Step 4: Validate generic arguments if present
    │       └── Step 5: Return resolved NamedTypeAST
    │
    ├── ArrayTypeAST
    │   └── resolveArrayType(type, ctx)
    │       ├── Step 1: Resolve the element type
    │       │   └── resolveType(type->element) → must succeed
    │       ├── Step 2: Validate element type
    │       │   ├── if element is RefTypeAST: error (reference in array)
    │       │   └── if element is ArrayTypeAST (slice): error (slice in array)
    │       └── Step 3: Validate context if this is a slice type
    │           └── if type->isSlice(): validateBorrowedContext(type, ctx)
    │
    ├── NullableTypeAST
    │   └── resolveNullableType(type, ctx)
    │       ├── Step 1: Resolve the inner type
    │       │   └── resolveType(type->inner) → must succeed
    │       ├── Step 2: Validate inner type cannot be function
    │       │   └── if inner is FuncTypeAST: error (function cannot be nullable)
    │       └── Step 3: Validate inner type cannot be array
    │           └── if inner is ArrayTypeAST: error (array cannot be nullable)
    │
    ├── FallibleTypeAST
    │   └── resolveFallibleType(type, ctx)
    │       ├── Step 1: Resolve the inner type
    │       ├── Step 2: Validate inner type cannot be function
    │       └── Step 3: Validate inner type cannot be array
    │
    ├── CombinedTypeAST
    │   └── resolveCombinedType(type, ctx)
    │       ├── Step 1: Resolve the inner type
    │       ├── Step 2: Validate inner type cannot be function
    │       └── Step 3: Validate inner type cannot be array
    │
    ├── RefTypeAST
    │   └── resolveRefType(type, ctx)
    │       ├── Step 1: Resolve the inner type
    │       ├── Step 2: Validate inner type cannot be trait
    │       │   └── if isTraitType(inner, ctx): error (&Trait not allowed)
    │       └── Step 3: Apply Downward Flow Rule
    │           └── validateBorrowedContext(type, ctx)
    │               ├── if in struct field: error (&T cannot be stored)
    │               ├── if in array element: error (&T cannot be stored)
    │               ├── if in function return: error (&T cannot escape upward)
    │               └── if in closure capture: error (&T cannot be captured)
    │
    ├── PtrTypeAST
    │   └── resolvePtrType(type, ctx)
    │       ├── Step 1: Resolve the inner type
    │       └── Step 2: Return type (always valid structurally)
    │           └── Raw pointers are sealed conduits - FFI checks done separately
    │
    └── FuncTypeAST
        └── resolveFuncType(type, ctx)
            ├── Step 1: Resolve all parameter types
            │   └── for each param: resolveType(param->type) → must succeed
            ├── Step 2: Resolve return type (if present)
            │   └── resolveType(type->returnType) → must succeed
            ├── Step 3: Validate return type cannot be borrowed
            │   ├── if isBorrowedType(returnType): error
            │   └── if returnType is RefTypeAST: error
            ├── Step 4: Validate return type cannot be trait
            │   └── if isTraitType(returnType, ctx): error (trait cannot be returned)
            └── Step 5: Recursively resolve curried return type
                └── if returnType is FuncTypeAST: resolveFuncType(returnType)
│
├── // ERROR HANDLING
│   └── On any error: report via ctx.diagnostics.error() and return nullptr
│
└── // RETURN VALUE CONVENTIONS
    ├── nullptr: Type does not exist (hard error)
    │   ├── Type lookup failed (undeclared)
    │   ├── Generic arity mismatch
    │   └── Module/type not found
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

### Type Comparison and Assignability (`SemaCompare.cpp`)

The `SemaCompare` module provides type equality checking, assignability validation, and type predicate functions used throughout semantic analysis.

**Purpose:**
- Compares two types for structural equality
- Determines if a value of one type can be assigned to another
- Provides type predicate functions (`isIntegerType`, `isNullableType`, etc.)
- Validates switch case compatibility and FFI type compatibility

---

#### Type Equality (`typesEqual`)

```
typesEqual(a, b)
│
├── if (a == b) return true
├── if (!a || !b) return false
├── if (a->kind != b->kind) return false
│
└── switch (a->kind)
    ├── PrimitiveType → compare primitiveKind
    │
    ├── NamedType → compare name, genericArgs recursively
    │
    ├── NullableType/FallibleType/CombinedType/RefType/PtrType
    │   └── compare inner types recursively
    │
    ├── ArrayType → compare arrayKind, size, element recursively
    │
    └── FuncType → compare params (name, variadic, const, type), returnType
```

---

#### Type Unwrapping

```
unwrapNullable(type)
├── if type->isa<NullableTypeAST>() → return type->inner
├── if type->isa<CombinedTypeAST>() → return type->inner
└── return type

unwrapFallible(type)
├── if type->isa<FallibleTypeAST>() → return type->inner
├── if type->isa<CombinedTypeAST>() → return type->inner
└── return type
```

---

#### Assignability (`isAssignable`)

```
isAssignable(target, source, ctx)
│
├── // 1. Identical types
├── if (typesEqual(target, source)) return true
│
├── // 2. Numeric conversions
├── // 2a. Integer → Float (safe, always allowed)
├── if (isFloatType(target) && isIntegerType(source)) return true
│
├── // 2b. Integer → Integer (different sizes, safe promotion only)
├── if (isIntegerType(target) && isIntegerType(source))
│   └── return isIntegerPromotionSafe(target, source, ctx)
│
├── // 2c. Float → Integer (unsafe, reject)
├── if (isIntegerType(target) && isFloatType(source)) return false
│
├── // 3. T → T? (widening to nullable)
├── if (target->isa<NullableTypeAST>())
│   └── return isAssignable(target->inner, source, ctx)
│
├── // 4. T → T! (widening to fallible)
├── if (target->isa<FallibleTypeAST>())
│   └── return isAssignable(target->inner, source, ctx)
│
├── // 5. T → T?! (widening to combined)
├── if (target->isa<CombinedTypeAST>())
│   ├── return isAssignable(target->inner, source, ctx)
│   ├── if (source->isa<NullableTypeAST>() && isAssignable(inner, source->inner)) return true
│   └── if (source->isa<FallibleTypeAST>() && isAssignable(inner, source->inner)) return true
│
└── // 6. Trait conformance
    if (isTraitType(target, ctx) && !isTraitType(source, ctx))
        └── return isTraitConformant(source, traitDecl, ctx)
```

**Assignability Rules Summary:**

| Source → Target    | Allowed? | Notes                        |
| ------------------ | -------- | ---------------------------- |
| `int` → `float`    | ✅ Yes    | Safe widening                |
| `int8` → `int32`   | ✅ Yes    | Safe promotion               |
| `int32` → `int8`   | ❌ No     | Requires explicit conversion |
| `float` → `int`    | ❌ No     | Requires explicit conversion |
| `T` → `T?`         | ✅ Yes    | Widening to nullable         |
| `T` → `T!`         | ✅ Yes    | Widening to fallible         |
| `T` → `T?!`        | ✅ Yes    | Widening to combined         |
| `T?` → `T?!`       | ✅ Yes    | Widening to combined         |
| `T!` → `T?!`       | ✅ Yes    | Widening to combined         |
| `Struct` → `Trait` | ✅ Yes    | If struct implements trait   |
| `Trait` → `Struct` | ❌ No     | Cannot narrow from trait     |

---

#### Numeric Type Helpers

```
getIntegerBitWidth(type)
├── PrimitiveKind::Int8/Uint8/Byte/Ubyte → 8
├── PrimitiveKind::Int16/Uint16/Short/Ushort → 16
├── PrimitiveKind::Int32/Uint32/Int/Uint → 32
├── PrimitiveKind::Int64/Uint64/Long/Ulong → 64
└── default → 0

getLargerIntegerType(a, b, ctx)
├── if (!isIntegerType(a) || !isIntegerType(b)) return nullptr
└── return the type with larger bit width

isIntegerPromotionSafe(target, source, ctx)
├── if (!isIntegerType(target) || !isIntegerType(source)) return false
└── return targetBits >= sourceBits
```

---

#### Type Predicates

```
Type Predicates:
├── isNullableType(type)      → type->isa<NullableTypeAST>() || type->isa<CombinedTypeAST>()
├── isFallibleType(type)      → type->isa<FallibleTypeAST>() || type->isa<CombinedTypeAST>()
├── isReferenceType(type)     → type->isa<RefTypeAST>()
├── isPointerType(type)       → type->isa<PtrTypeAST>()
├── isPrimitiveType(type)     → type->isa<PrimitiveTypeAST>()
├── isBoolType(type)          → PrimitiveKind::Bool
├── isIntegerType(type)       → PrimitiveKind in integer set
├── isFloatType(type)         → PrimitiveKind in float set
├── isNumericType(type)       → isIntegerType() || isFloatType()
├── isStringType(type)        → PrimitiveKind::String
├── isCharType(type)          → PrimitiveKind::Char
│
├── isStructType(type, ctx)   → NamedType resolves to StructDecl
├── isEnumType(type, ctx)     → NamedType resolves to EnumDecl
├── isTraitType(type, ctx)    → NamedType resolves to TraitDecl
├── isGenericParamType(type, ctx) → NamedType is a generic parameter
│
├── isValidSwitchType(type, ctx)
│   └── isIntegerType() || isBoolType() || isCharType() || isStringType() || isEnumType()
│
└── isSwitchCaseCompatible(value, subjectType, ctx)
    ├── Enum → FieldAccessExpr matching an enum variant
    ├── Integer → Int/Hex/Binary literal
    ├── Bool → True/False literal
    ├── Char → Char literal
    └── String → String/RawString literal
```

---

#### FFI Compatibility (`isValidFFIType`)

```
isValidFFIType(type, ctx)
│
├── PrimitiveType → true
│
├── PtrTypeAST → 
│   ├── if inner is FuncTypeAST: validate params and return recursively
│   ├── if inner is ArrayTypeAST: false
│   ├── if inner is nullable/fallible: false
│   ├── if inner is RefTypeAST: false
│   └── return isValidFFIType(inner, ctx)
│
├── NamedTypeAST →
│   ├── TraitDeclAST → false
│   ├── StructDeclAST → validate all fields recursively
│   └── EnumDeclAST → true
│
├── ArrayTypeAST → isValidFFIType(element, ctx)
│
├── NullableType/FallibleType → false
├── FuncTypeAST → false
├── RefTypeAST → false
│
└── default → false
```

**FFI-Compatible Types:**

| Type                                      | FFI Compatible? | Notes                            |
| ----------------------------------------- | --------------- | -------------------------------- |
| Primitives (`int`, `float`, `bool`, etc.) | ✅ Yes           | C-compatible                     |
| `*T` (raw pointer)                        | ✅ Yes           | Opaque pointer                   |
| `struct` (with FFI-compatible fields)     | ✅ Yes           | Layout matches C                 |
| `enum`                                    | ✅ Yes           | Integer backing                  |
| `[*]T` (dynamic array)                    | ✅ Yes           | Pointer to data                  |
| `[N]T` (fixed array)                      | ✅ Yes           | Contiguous layout                |
| `T?`, `T!`, `T?!`                         | ❌ No            | Tagged types not FFI-compatible  |
| `&T` (reference)                          | ❌ No            | Borrowed types                   |
| `(T) -> R` (function)                     | ❌ No            | Function pointers not supported  |
| `trait`                                   | ❌ No            | Trait objects not FFI-compatible |

---

### Type Validation (`SemaValidate.cpp`)

The `SemaValidate` module provides validation rules for declarations and constructs that require more complex checks than simple type resolution.

**Purpose:**
- Validates const declarations (must be definite, have initializer)
- Validates trait implementations (field existence, type compatibility, conflicts)
- Validates generic arguments (arity, constraints)
- Validates foreign function declarations (ABI, FFI compatibility)

---

#### Const Validation

```
validateConstType(type, name, kind, ctx)
│
├── if (isNullableType(type) || isFallibleType(type))
│   └── error: const must be definite (not nullable or fallible)
│
├── if (type->isa<CombinedTypeAST>())
│   └── error: const cannot be combined (T?!)
│
└── if (isBorrowedType(type))
    └── error: const cannot be borrowed (&T or [_]T)

validateConstInitializer(hasInit, name, kind, ctx)
├── if (!hasInit)
│   └── error: const must have an initializer
└── return true
```

**Const Validation Rules:**

| Rule                      | Description                          |
| ------------------------- | ------------------------------------ |
| **Must Be Definite**      | Const cannot be `T?`, `T!`, or `T?!` |
| **Cannot Be Borrowed**    | Const cannot be `&T` or `[_]T`       |
| **Must Have Initializer** | Const must be assigned a value       |

---

#### Trait Validation

```
validateTraitImplementation(structDecl, traitDecl, ctx)
│
├── Build map of struct fields by name
│
├── for each traitField in traitDecl->fields:
│   ├── // 1. Field exists in struct
│   ├── if field not found in structFields
│   │   └── error: struct is missing field required by trait
│   │
│   ├── // 2. Const-ness compatibility
│   ├── if traitField is const and structField is not const
│   │   └── error: trait requires const, struct declares mutable
│   │
│   ├── // 3. Type compatibility
│   ├── if traitField is const:
│   │   └── require typesEqual(structField->type, traitField->type)
│   └── if traitField is not const:
│       └── require isAssignable(traitField->type, structField->type, ctx)
│
└── return isValid
```

```
validateAllTraitImplementations(structDecl, ctx)
│
├── if structDecl->traitRefs.empty() → return true
│
├── // 1. Check for field conflicts across traits
├── conflicts = checkTraitFieldConflictsInternal(structDecl, ctx)
│
├── // 2. Validate each trait implementation
├── for each traitRef in structDecl->traitRefs:
│   ├── trait = resolveTraitRef(traitRef, ctx)
│   ├── if !trait → continue
│   └── if !validateSingleTraitImplementationInternal(structDecl, trait, ctx)
│       └── allValid = false
│
└── return allValid && !conflicts
```

```
checkTraitFieldConflictsInternal(structDecl, ctx)
│
├── Build map: fieldName → vector of {trait, isConst, type}
│
├── for each conflict where fieldName has multiple requirements:
│   ├── if const-ness differs
│   │   └── error: field has conflicting const requirements
│   ├── if types differ
│   │   └── error: field has conflicting types across traits
│   └── hasConflict = true
│
└── return !hasConflict
```

**Trait Implementation Rules:**

| Rule                   | Description                                                              |
| ---------------------- | ------------------------------------------------------------------------ |
| **Field Exists**       | Struct must have every field defined in the trait                        |
| **Const Matching**     | Const trait fields require const struct fields                           |
| **Type Compatibility** | Non-const fields require assignability; const fields require exact match |
| **No Conflicts**       | Same field name across multiple traits must have compatible requirements |
| **No Borrowed Types**  | Trait fields cannot be borrowed types (`&T`, `[_]T`)                     |

---

#### Generic Validation

```
validateGenericArguments(args, params, useSite, ctx)
│
├── if (args.size() != params.size())
│   └── error: expected N generic arguments, got M
│
├── for each i in args:
│   ├── resolvedArg = resolveType(args[i], ctx)
│   ├── if !resolvedArg → error: invalid generic argument
│   │
│   ├── // Generic arguments cannot be borrowed types
│   ├── if (isBorrowedType(resolvedArg))
│   │   └── error: generic argument cannot be borrowed type
│   │
│   └── if !validateParamConstraints(resolvedArg, params[i], ctx)
│       └── error: type does not implement trait constraint
│
└── return allValid
```

```
validateGenericParameterUsage(params, types, useSite, ctx)
│
├── // Collect all generic parameter references in types
├── for each type in types:
│   └── findParams(type) → collect used parameter names
│
├── for each param in params:
│   └── if param not in usedParams
│       └── error: generic parameter not used in declaration
│
└── return allUsed
```

**Generic Validation Rules:**

| Rule                  | Description                                             |
| --------------------- | ------------------------------------------------------- |
| **Arity Match**       | Number of arguments must match number of parameters     |
| **No Borrowed Types** | Generic arguments cannot be `&T` or `[_]T`              |
| **Trait Constraints** | Arguments must satisfy all trait constraints            |
| **All Params Used**   | Every generic parameter must be used in the declaration |

---

#### FFI Validation (`validateForeignFunction`)

```
validateForeignFunction(decl, foreignAttr, ctx)
│
├── // 1. Validate ABI
├── if foreignAttr->args.empty()
│   └── error: @[foreign] requires ABI argument
├── abi = getStringLiteral(foreignAttr->args[0])
├── if abi != "C"
│   └── error: only "C" ABI is supported
│
├── // 2. Function must have no body
├── if (decl->body)
│   └── error: foreign function must have no body
│
├── // 3. Validate parameter types
├── for each param in funcType->params:
│   └── if !isValidFFIType(param->type, ctx)
│       └── error: parameter type is not FFI-compatible
│
├── // 4. Validate return type
├── if (returnType && !isValidFFIType(returnType, ctx))
│   └── error: return type is not FFI-compatible
│
└── // 5. No generic parameters
    if !decl->genericParams.empty()
        └── error: foreign function cannot have generic parameters
```

**Foreign Function Validation Rules:**

| Rule                     | Description                                           |
| ------------------------ | ----------------------------------------------------- |
| **ABI Must Be "C"**      | Only C ABI is supported                               |
| **No Body**              | Foreign functions are external implementations        |
| **FFI-Compatible Types** | All parameters and return type must be FFI-compatible |
| **No Generics**          | Foreign functions cannot be generic                   |
| **No Variadic**          | Variadic parameters are not supported                 |
| **No Const**             | Foreign functions cannot be `const` (compile-time)    |

---

### Type Narrowing (`TypeNarrowHelpers.cpp`)

Type narrowing is a flow-sensitive analysis that refines variable types based on conditional checks and operations.

**Narrowing Entry Points:**

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Narrowing Entry Points                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌───────────────────────────────────────┐  ┌─────────────────────────────┐ │
│  │         If Statement (Condition)      │  │      Await/Join Statements  │ │
│  ├───────────────────────────────────────┤  ├─────────────────────────────┤ │
│  │  x != nil  → direct (THEN branch)     │  │  await x → Future<T> → T    │ │
│  │  x == nil  → inverse (ELSE/rest)      │  │  join x  → Thread<T> → T    │ │
│  │  x != err  → direct (THEN branch)     │  │                             │ │
│  │  x == err  → inverse (ELSE/rest)      │  │                             │ │
│  └───────────────────────────────────────┘  └─────────────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### IF STATEMENT ENTRY

```cpp
resolveIfStmt()
│
├── ScopedIfCondition(ctx)  // Sets isIfConditionCtx = true
│
├── resolveExprWithTarget(condition, boolType)
│   │
│   └── resolveBinaryExpr()
│       │
│       └── if ctx.stack.isIfConditionCtx():
│           │
│           └── info = detectNarrowingPattern(expr, ctx)
│               │
│               └── ctx.stack.setPendingNarrowing(info)
│
├── info = ctx.stack.getPendingNarrowing()
│
├── THEN BRANCH:
│   └── ScopedNarrowing(ctx, info.narrowings, false)
│       └── pushNarrowingLevel(false)
│           └── narrowVariable(name, type)  // Apply each narrowing
│
└── ELSE BRANCH:
    └── ScopedNarrowing(ctx, info.narrowings, true)
        └── pushNarrowingLevel(true)
            └── narrowVariable(name, type)  // Apply each narrowing
```

#### AWAIT/JOIN STATEMENT ENTRY

```cpp
resolveAwaitStmt() / resolveJoinStmt()
│
└── for each target:
    │
    ├── if hasPendingAsync(targetName):
    │   ├── futureType = decl->type->as<FutureTypeAST>()
    │   ├── innerType = futureType->inner
    │   └── ctx.stack.narrowVariable(targetName, innerType)
    │       └── pushNarrowingLevel(false)
    │           └── narrowedTypes[targetName] = innerType
    │
    └── ctx.resolveAsync(targetName)  // Remove from pending
```

#### CONDITION NARROWING EXTRACTION

```cpp
extractNarrowingsFromCondition(expr, ctx, outIsValidMixed)
│
├── // 1. Handle `or` at top level
│   Pattern: a == nil or b == nil
│   ├── left = extractNarrowingsFromCondition(left)
│   ├── right = extractNarrowingsFromCondition(right)
│   ├── If both have narrowing and different isEquality → invalid (reject)
│   └── Merge both narrowings (OR combines both possibilities)
│
├── // 2. Handle `and` at top level
│   Pattern: a == nil and b == nil
│   └── Return empty (no narrowing - unsound because inverse would be OR)
│
├── // 3. Handle simple binary comparison
│   └── detectSingleNarrowing(expr)
│       ├── x == nil → narrows x to inner type, isEquality = true
│       ├── x != nil → narrows x to inner type, isEquality = false
│       ├── x == err → narrows x to inner type, isEquality = true
│       └── x != err → narrows x to inner type, isEquality = false
│
└── // 4. Handle `not x`
    Pattern: not x
    └── Inverse narrowing: x is nil/false, isEquality = true
```

```cpp
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
```

```cpp
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

#### NARROWING PATTERNS

**Pattern 1: Nullable/Fallible Checks**

```
x != nil  → Direct Narrowing (applies to THEN branch)
x == nil  → Inverse Narrowing (applies to ELSE or rest)

let x int? = getValue()
if x != nil {
    // x is int (narrowed from int?)
    use(x) // safe
}
// x is int? (narrowing level popped)
```

```
x != err  → Direct Narrowing (applies to THEN branch)
x == err  → Inverse Narrowing (applies to ELSE or rest)
```

**Pattern 2: Combined Conditions**

```
x != nil and y != nil  → All != checks (direct narrowing)
if x != nil and y != nil {
    // x is int, y is string (both narrowed)
}
```

```
x == nil or y == nil  → All == checks (inverse narrowing)
if x == nil or y == nil {
    return
}
// x is int OR y is string (both narrowed)
```

```
x != nil and y == nil  → ❌ REJECTED (mixed operators)
if x != nil and y == nil {
    // ERROR: mixed '!=' and '==' in condition
    // Cannot determine narrowing semantics
}
```

**Pattern 3: Await/Join Narrowing**

```
await x  → Narrow Future<T> → T (applies to rest of block)
async result int = fetchValue()  // result is Future<int>
await result                      // Narrow to int
// result is int here
use(result) // safe
```

```
join x   → Narrow Thread<T> → T (applies to rest of block)
spawn result int = computeValue()  // result is Thread<int>
join result                         // Narrow to int
// result is int here
use(result) // safe
```

#### NARROWING STACK MANAGEMENT

The narrowing stack tracks flow-sensitive type refinements across nested scopes.

```cpp
Narrowing Stack Structure:

Level 3 (innermost)  { x: int, y: string }
Level 2              { x: int? }
Level 1              { }

Lookup "x" → Level 3 has x → returns int
Lookup "y" → Level 3 has y → returns string
Lookup "z" → No level has z → returns nullptr
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

#### PENDING INVERSE NARROWING

For standalone if statements with early exit (`if x == nil { return }`), the inverse narrowing is stored as pending and applied to the rest of the block.

```cpp
Step 1: Condition Analysis
if x == nil {
    return
}
// Pending inverse narrowing captured: x → int

Step 2: Store Pending Inverse Narrowing
if !stmt->elseBranch && thenReturns &&
   hasNarrowing && info.isEquality {
    ctx.stack.setPendingInverseNarrowing(info)
}

Step 3: Apply Pending Narrowing on Block Entry
resolveBlock() {
    if ctx.stack.hasPendingInverseNarrowing() {
        info = ctx.stack.getPendingInverseNarrowing()
        ctx.stack.pushNarrowingLevel(true)
        for (name, type : info.narrowings) {
            ctx.stack.narrowVariable(name, type)
        }
        ctx.stack.clearPendingInverseNarrowing()
    }
    // ... resolve block statements ...
}
```

#### EFFECTIVE TYPE LOOKUP

When resolving an identifier, the compiler checks the narrowing stack for a narrowed type before falling back to the declaration's type.

```cpp
getEffectiveType(decl, name)
│
├── narrowedType = stack.getNarrowedType(name)
├── if narrowedType:
│   └── return narrowedType
│
└── return decl->type

Usage in resolveIdentifierExpr():
TypeAST* effectiveType = ctx.getEffectiveType(decl, name)
└── Returns narrowed type if available, otherwise decl->type
```

#### NARROWING RULES SUMMARY

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

---

### Capture Analysis (`CaptureAnalysis.cpp`)

Capture analysis detects which variables from outer scopes are referenced inside a closure (anonymous function or nested function). It also performs escape analysis to determine if closures must be heap-allocated.

**Purpose:**
- Detects which variables are captured by closures and nested functions
- Validates that captured variables are not borrowed types (`&T`, `[_]T`)
- Validates that captured variables are not linear types (`Future<T>`, `Thread<T>`)
- Stores capture information on the AST node for code generation
- Marks closures as returned when they escape via function return

```cpp
analyzeCaptures(expr, ctx)           // Analyze anonymous function
│
├── // EARLY EXIT
│   └── if expr->body is null: return
│
├── // COLLECT CAPTURES
│   └── walkStmt(expr->body)
│       │
│       └── // WALK AST with localScopes tracking
│           │
│           ├── // BlockStmt: pushLocalScope() / popLocalScope()
│           │
│           ├── // DeclStmt: register var after walking init → declareLocal()
│           │
│           ├── // ForStmt: pushLocalScope() for binders
│           │
│           ├── // IdentifierExpr
│           │   │   Check if this identifier is a capture
│           │   ├── if ctx.isModuleMember(name) → NOT CAPTURE
│           │   ├── if isOwnParam(name) → NOT CAPTURE
│           │   ├── if isLocallyDeclared(name) → NOT CAPTURE
│           │   ├── if ctx.isGenericParam(name) → NOT CAPTURE
│           │   ├── if !ctx.lookupValue(name) → NOT CAPTURE
│           │   └── else → CAPTURE
│           │
│           └── // AnonFuncExpr (nested closure)
│               │   Propagate captures upward
│               └── propagateCapture(childCapture)
│
├── // VALIDATE CAPTURES (validateAndAddCapture)
│   └── for each capture:
│       ├── // Deduplicate: if seenCaptures.contains(name) → skip
│       ├── // Rule 1: No borrowed types
│       │   └── if isBorrowedType(decl->type):
│       │       └── error: closure cannot capture borrowed type
│       ├── // Rule 2: No linear types
│       │   └── if decl->type is FutureTypeAST or ThreadTypeAST:
│       │       └── error: closure cannot capture linear type
│       └── // Add to capture list
│           └── CapturedVariable{decl, byReference, index}
│
└── // STORE RESULT
    └── expr->captures = captureSpan, expr->hasClosure = true
```

```cpp
analyzeCaptures(FuncDeclAST* func, ctx)    // Analyze nested function
│
├── // EARLY EXIT
│   ├── if !func || !func->body → return
│   └── if ctx.getClosureDepth() == 0 → return (top-level function cannot capture)
│
├── // COLLECT CAPTURES (same as anonymous function)
│   └── walkStmt(func->body)
│
└── // STORE RESULT
    └── func->captures = captureSpan, func->hasClosure = true
```

```cpp
markClosureIfEscaping(expr, ctx)         // Detect closures returned from functions
│
├── // PURPOSE
│   │   Detects when a closure is returned from a function and must be
│   │   heap-allocated because it outlives the function call.
│   └── Uses `isReturned` field (not `isEscaping`)
│
├── // DISPATCH BY EXPRESSION KIND
│
    ├── AnonFuncExpr
    │   └── expr->isReturned = true
    │
    ├── IdentifierExpr
    │   ├── if !ctx.isModuleMember(id->name):
    │   │   └── funcDecl->isReturned = true
    │   └── else: return (module members are static)
    │
    ├── ModuleAccessExpr
    │   └── return (static member - no escaping needed)
    │
    ├── FieldAccessExpr
    │   └── if object is module member: return
    │       else: conservative mark
    │
    ├── CallExpr
    │   └── resolveCalleeOrError() → may return closure
    │
    ├── BinaryExpr → recurse into left and right
    ├── IfExpr → recurse into then and else
    ├── ArrayLiteralExpr → recurse into all elements
    ├── StructLiteralExpr → recurse into all field inits
    ├── PipelineExpr → recurse into seed and steps
    └── ComposeExpr → recurse into left and operands
```

**Capture Detection Walkthrough:**

```cpp
const makeCounter (start int) -> (int) -> int = {
    let count int = start

    return (step int) -> int {
        count += step
        return count
    }
}

Step 1: analyzeCaptures() called on the inner anonymous function

Walk AST of the anonymous function body:
├── BinaryExpr (count += step)
│   ├── left: IdentifierExpr("count")
│   │   ├── ctx.isModuleMember("count")? → false
│   │   ├── isOwnParam("count")? → false
│   │   ├── isLocallyDeclared("count")? → false (no let/const in body)
│   │   ├── ctx.isGenericParam("count")? → false
│   │   ├── ctx.lookupValue("count")? → true (from outer scope)
│   │   └── → CAPTURE
│   └── right: IdentifierExpr("step")
│       ├── isOwnParam("step")? → true (parameter)
│       └── → NOT CAPTURE
└── ReturnStmt (return count)
    └── IdentifierExpr("count")
        ├── isOwnParam("count")? → false
        ├── isLocallyDeclared("count")? → false
        └── → CAPTURE (already captured)

Step 2: Validate captures
└── count is not borrowed type → valid capture

Step 3: Store result
└── expr->captures = count, expr->hasClosure = true

Step 4: markClosureIfEscaping() called on return expression
└── expr is AnonFuncExpr → expr->isReturned = true
    └── Closure escapes → heap-allocated in code generation
```

**Transitive Capture Propagation (Multi-Level Closures):**

```cpp
// Example: three-level nested closure
const outer () -> (int) -> int = {
    let x int = 10
    return (a int) -> int {
        // This closure captures x (from outer) → stored in its capture list
        return (b int) -> int {
            // This innermost closure captures x (from outer's outer)
            // It DOES NOT see x in its own capture list - it's not in its
            // immediate outer closure's parameters or locals.
            // 
            // propagateCapture() pulls x from the intermediate closure's
            // capture list and adds it to the intermediate closure's own
            // capture list, so CodeGen can properly build all environments.
            return x + a + b
        }
    }
}
```

**Transitive Propagation Flow:**
```
1. Innermost closure captures x from outer's outer
2. Inner closure's capture list has x (from its analysis)
3. Outer closure's walk sees AnonFuncExpr (inner)
4. propagateCapture(x) adds x to outer's capture list
5. All closures have the variables they need in their own capture lists
```

**Capture Storage on AST Nodes:**

```cpp
// AnonFuncExprAST fields for capture analysis
struct AnonFuncExprAST {
    bool hasClosure = false;                    // true if captures any variables
    ArenaSpan<CapturedVariable> captures;       // list of captured variables
    bool isReturned = false;                    // true if returned from function
};

// FuncDeclAST fields for capture analysis
struct FuncDeclAST {
    bool hasClosure = false;                    // true if nested function captures
    ArenaSpan<CapturedVariable> captures;       // list of captured variables
    bool isReturned = false;                    // true if returned from function
};

// CapturedVariable structure (actual implementation)
struct CapturedVariable {
    ValueDeclAST* decl;                         // variable declaration
    bool byReference;                           // true if captured by reference
    size_t index;                               // position in environment struct
};
```

**Key Relationships:**

| Component                              | Responsibility                                 | Called From                                     |
| -------------------------------------- | ---------------------------------------------- | ----------------------------------------------- |
| `analyzeCaptures()`                    | Main entry point for capture analysis          | `resolveAnonFuncExpr()`, `resolveFuncDecl()`    |
| `walkStmt()` / `walkExpr()`            | Walks AST to find identifier references        | `analyzeCaptures()`                             |
| `isCapture()`                          | Determines if identifier is a capture          | `processIdentifier()`                           |
| `isLocallyDeclared()`                  | Checks if variable is declared in closure body | `isCapture()`                                   |
| `pushLocalScope()` / `popLocalScope()` | Tracks block-scoped declarations               | `walkStmt()` for BlockStmt, ForStmt             |
| `declareLocal()`                       | Registers a local variable in current scope    | `walkStmt()` for DeclStmt, AsyncStmt, SpawnStmt |
| `validateAndAddCapture()`              | Validates capture rules and adds to list       | `processIdentifier()`, `propagateCapture()`     |
| `propagateCapture()`                   | Propagates captures upward through nesting     | `walkExpr()` for nested AnonFuncExpr            |
| `markClosureIfEscaping()`              | Detects escaping closures for heap allocation  | `resolveReturnStmt()`                           |

**Important Rules:**

| Rule                      | Description                                                     | Example                                                                          |
| ------------------------- | --------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| **No Borrowed Types**     | Closures cannot capture `&T` or `[_]T`                          | `let x &int = ...; return (y int) -> int { return *x + y }` ❌                    |
| **No Linear Types**       | Closures cannot capture `Future<T>` or `Thread<T>`              | `async result int = fetch(); return () -> int { return await result }` ❌         |
| **Module Members**        | Module-level globals are not captured (they're global)          | `const PI f64 = 3.14; return (r f64) -> f64 { return PI * r * r }` ✅             |
| **Generic Parameters**    | Generic parameters are not captured (resolved at instantiation) | `<T> const id (v T) -> (T) -> T = { return (x T) -> T { return x } }` ✅          |
| **Local Variables**       | Variables from outer function scopes are captured               | `let count int = 0; return () -> int { return count }` ✅                         |
| **Escaping Closures**     | Closures returned from functions are heap-allocated             | `return (x int) -> int { return x + 1 }` → heap allocated (`isReturned = true`)  |
| **Non-Escaping Closures** | Closures used locally are stack-allocated                       | `let f (int) -> int = (x int) -> int { return x + 1 }; use(f)` → stack allocated |

**Key Implementation Notes:**

1. **Capture Detection**: The analyzer walks the AST recursively, checking every `IdentifierExpr` node to determine if it references a variable from an outer scope.

2. **Scope Determination**: A variable is considered a capture if it is:
   - Not a module member (global)
   - Not a parameter of the current function/closure (`isOwnParam`)
   - Not declared within the body being walked (`isLocallyDeclared`)
   - Not a generic parameter
   - Found in some outer scope via `ctx.lookupValue()`

3. **Local Scope Tracking**: `localScopes` is a stack of block-scoped frames that tracks variable declarations within the closure body. This is necessary because capture analysis runs as a second pass after the first pass's scopes have been popped. Without this, a name declared inside the body that shadows an outer variable would incorrectly resolve to the outer one.

4. **Transitive Capture Propagation**: When a nested closure captures a variable, the enclosing closure must also capture it (even if it doesn't reference it directly) so CodeGen's flat value map has the correct value when building the nested closure's environment.

5. **Validation Rules**: Captures are validated immediately when detected:
   - Borrowed types (`&T`, `[_]T`) are rejected with a clear error message
   - Linear types (`Future<T>`, `Thread<T>`) are rejected

6. **Escape Analysis**: `markClosureIfEscaping()` is called during return statement resolution to detect closures that outlive the function call and must be heap-allocated. Uses `isReturned` field, not `isEscaping`.

---

### Const Evaluation (`ConstEvaluator`)

The const evaluator is responsible for evaluating expressions at compile-time. It is invoked during Phase 2 of semantic analysis when resolving `const` declarations.

**ConstEvaluator Overview:**

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                          ConstEvaluator Overview                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────┐  ┌─────────────────────┐  ┌─────────────────────┐  │
│  │   Constant Values   │  │   Control Flow      │  │   Type System       │  │
│  ├─────────────────────┤  ├─────────────────────┤  ├─────────────────────┤  │
│  │ • Bool (true/false) │  │ • if/else           │  │ • Int → int64       │  │
│  │ • Int (int64)       │  │ • while loops       │  │ • Float → double    │  │
│  │ • Float (double)    │  │ • for loops (range) │  │ • String            │  │
│  │ • String            │  │ • switch/case       │  │ • Struct            │  │
│  │ • Char              │  │ • return            │  │ • Array             │  │
│  │ • Nil / Err         │  │ • block scoping     │  │                     │  │
│  │ • Struct / Array    │  │                     │  │                     │  │
│  └─────────────────────┘  └─────────────────────┘  └─────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### ENTRY POINTS

```cpp
evaluateDecl(ctx, decl)
│
├── Check: decl->init exists
├── Check: circular dependency (m_evaluating.contains(decl))
├── Push: EvaluationGuard (mark as evaluating)
├── Push: Scope (make variable visible to itself)
├── result = evaluate(ctx, decl->init, decl->type)
├── Pop: Scope
└── Return: result
```

```cpp
evaluate(ctx, expr, targetType)
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

#### LITERAL EVALUATION

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

#### IDENTIFIER EVALUATION

```cpp
evalIdentifier(ctx, expr)
│
├── if name == "_" → return unknown()
│
├── decl = ctx.lookupValue(name)
│
├── // Variable
│   if VarDecl:
│       if var->init && var->init->isConst → return constValue
│       if var->keyword == Const:
│           if m_evaluating.contains(var) → report cycle
│           return evaluate(ctx, var->init, var->type)
│       return unknown() (non-const variable)
│
├── // Function
│   if FuncDecl:
│       if func->keyword != Const → return unknown()
│       return ConstantValue(func)  (function pointer)
│
├── // Enum Variant
│   if EnumVariantAST:
│       return ConstantValue(variant->value)
│
└── // Parameter
    if ParamAST:
        return unknown() (value bound during execution)
```

#### BINARY EXPRESSION EVALUATION

```cpp
evalBinary(ctx, expr, targetType)
│
├── // Type Narrowing (if condition context)
│   if ctx.stack.isIfConditionCtx():
│       info = detectNarrowingPattern(expr, ctx)
│       if info.hasNarrowing:
│           ctx.stack.setPendingNarrowing(info)
│           return unknown()  // condition is const
│
├── left = evaluate(ctx, expr->left, targetType)
│   if error/unknown → return
│
├── // Short-circuit Logic
│   if expr->op == And and left.isBool() and !left.asBool() → return false
│   if expr->op == Or  and left.isBool() and left.asBool()  → return true
│
├── right = evaluate(ctx, expr->right, targetType)
│   if error/unknown → return
│
└── return evalBinaryOp(ctx, op, left, right, expr, targetType)
```

```cpp
evalBinaryOp(ctx, op, left, right, node, targetType)
│
├── // Arithmetic (numeric promotion)
│   if op in (Add, Sub, Mul, Div, Mod, Pow):
│       if both numeric:
│           promote int → float if mixed
│           check overflow (Add/Sub/Mul/Div/Mod)
│           check division by zero (Div/Mod)
│           return result
│       else error
│
├── // String Concatenation
│   if op == Add and left.isString() and right.isString():
│       return concat(left, right)
│
├── // Comparison
│   if op in (Eq, Ne, Lt, Gt, Le, Ge):
│       if kinds match:
│           return compare(left, right)
│       else error
│
├── // Logical
│   if op in (And, Or):
│       if both bool: return left && right or left || right
│       else error
│
└── // Bitwise
    if op in (BitAnd, BitOr, BitXor, Shl, Shr):
        if both int:
            check shift bounds (Shl/Shr)
            return bitwise operation
        else error
```

#### UNARY EXPRESSION EVALUATION

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

#### CALL EXPRESSION (CONST FUNCTION)

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
├── // Evaluate arguments
│   for each arg in expr->args:
│       val = evaluate(ctx, arg)
│       if error/unknown → return
│
└── return executeFunction(ctx, func, args)
```

```cpp
executeFunction(ctx, func, args)
│
├── // Recursion guard
│   if recursionDepth >= MAX_RECURSION (1000) → error
│   recursionDepth++
│   DepthGuard (auto-decrement on exit)
│
├── // Setup context
│   ConstFunctionContext:
│       ctx.stack.pushFunction(func, func->funcType->returnType)
│       ctx.pushScope()
│
├── // Bind arguments
│   for each parameter:
│       param->type = getConstantType(args[index++])
│
├── // Execute body
│   if func->body:
│       result = executeStmt(ctx, func->body)
│   else:
│       error: const function has no body
│
├── // Check return type
│   if returnType != void:
│       if result.isVoid() → error (non-void function returns nothing)
│   else:
│       if !result.isVoid() → error (void function returns value)
│
└── return result
```

#### STATEMENT EXECUTION (FOR CONST FUNCTIONS)

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
```

```cpp
executeBlock(ctx, block)
│
├── ctx.pushScope()
├── for each stmt in block->stmts:
│   ├── result = executeStmt(ctx, stmt)
│   ├── if error/unknown → break
│   └── if !result.isVoid() → break (return/break/continue)
├── ctx.popScope()
└── return result
```

```cpp
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
```

```cpp
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
```

```cpp
executeFor(ctx, stmt)
│
├── if iterable is RangeExprAST:
│   ├── lo = evaluateAsInt(ctx, range->lo)
│   ├── hi = evaluateAsInt(ctx, range->hi)
│   ├── if lo or hi missing → return unknown()
│   ├── if inclusive: validate lo <= hi
│   ├── if exclusive: validate lo < hi
│   │   if invalid → error
│   ├── step = 1
│   │   if stmt->step:
│   │       step = evaluateAsInt(ctx, stmt->step)
│   │       if step missing or step <= 0 → error
│   ├── iterations = 0
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
```

```cpp
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
├── // Match cases
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
└── // Default
    if defaultBody:
        return executeStmt(ctx, defaultBody)
    return voidValue()
```

```cpp
executeReturn(ctx, stmt)
│
├── if stmt->value:
│   result = evaluate(ctx, stmt->value)
│   if error/unknown → return
│   return result
│
└── return voidValue()
```

```cpp
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

#### RAII GUARDS

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                             RAII Guards Overview                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────┐  ┌──────────────────────┐  ┌─────────────────────┐ │
│  │   EvaluationGuard   │  │ ConstFunctionContext │  │     DepthGuard      │ │
│  ├─────────────────────┤  ├──────────────────────┤  ├─────────────────────┤ │
│  │ Prevents circular   │  │ Sets up function     │  │ Prevents infinite   │ │
│  │ dependencies in     │  │ context for const    │  │ recursion in const  │ │
│  │ const declarations  │  │ function execution   │  │ function evaluation │ │
│  └─────────────────────┘  └──────────────────────┘  └─────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

```cpp
EvaluationGuard
│
├── // Purpose
│   Prevents infinite recursion in circular dependencies between const
│   declarations. Tracks which declarations are currently being evaluated.
│
├── // Construction
│   EvaluationGuard(m_evaluating, decl)
│   ├── m_evaluating.insert(decl)
│   └── (decl is now marked as "currently evaluating")
│
├── // Destruction
│   ~EvaluationGuard()
│   └── m_evaluating.erase(decl)
│
└── // Usage
    In evaluateDecl():
    ├── if m_evaluating.contains(decl):
    │   └── Report circular dependency error
    └── EvaluationGuard guard(m_evaluating, decl)
        // ... evaluate the declaration ...
        // Automatically removed when guard goes out of scope

    In evalIdentifier():
    └── if m_evaluating.contains(var):
        └── Report circular dependency error
```

```cpp
ConstFunctionContext
│
├── // Purpose
│   Sets up the semantic context for executing a const function. Pushes
│   a function context (for return validation) and a scope (for parameters).
│
├── // Construction
│   ConstFunctionContext(ctx, func)
│   ├── ctx.stack.pushFunction(func, func->funcType->returnType)
│   └── ctx.pushScope()
│
├── // Destruction
│   ~ConstFunctionContext()
│   ├── ctx.popScope()
│   └── ctx.stack.pop()
│
└── // Usage
    In executeFunction():
    └── ConstFunctionContext context(ctx, func)
        // ... bind parameters and execute body ...
        // Automatically cleaned up when context goes out of scope
```

```cpp
DepthGuard
│
├── // Purpose
│   Prevents infinite recursion in const function calls by tracking the
│   call stack depth.
│
├── // Construction
│   DepthGuard depthGuard{m_recursionDepth}
│   └── m_recursionDepth++
│
├── // Destruction
│   ~DepthGuard()
│   └── m_recursionDepth--
│
└── // Usage
    In executeFunction():
    ├── if m_recursionDepth >= MAX_RECURSION (1000):
    │   └── Error: exceeded maximum recursion depth
    └── DepthGuard depthGuard{m_recursionDepth}
        // ... execute function body ...
        // Automatically decremented when guard goes out of scope
```

#### INTERNAL STATE

```cpp
Internal State Overview:

┌──────────────────────────┐  ┌──────────────────────────┐
│      m_constDecls        │  │         m_deps           │
├──────────────────────────┤  ├──────────────────────────┤
│ All const declarations   │  │ Dependency graph:        │
│ (VarDecl + FuncDecl)     │  │ decl → [dependencies]    │
└──────────────────────────┘  └──────────────────────────┘

┌──────────────────────────┐  ┌──────────────────────────┐
│    m_evaluatedExprs      │  │      m_evaluating        │
├──────────────────────────┤  ├──────────────────────────┤
│ Cache of evaluated       │  │ Set of declarations      │
│ expressions (avoid       │  │ currently being          │
│ re-evaluation)           │  │ evaluated (cycle         │
│                          │  │ detection)               │
└──────────────────────────┘  └──────────────────────────┘

┌──────────────────────────┐
│     m_recursionDepth     │
├──────────────────────────┤
│ Current call stack depth │
│ for const functions      │
│ (MAX_RECURSION = 1000)   │
└──────────────────────────┘
```

#### CONSTANT VALUE TYPES

```
ConstantValue Structure:

┌─────────────────────────────────────────────────────────────────────────────┐
│                        ConstantValue                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│  kind  : Kind enum (Bool, Int, Float, String, Char, Nil, Err, ...)          │
│  value : variant (bool, int64_t, double, InternedString, ...)               │
│  type  : TypeAST* (cached semantic type)                                    │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                           Kind Enum                                         │
├─────────────────────────────────────────────────────────────────────────────┤
│  bool    │ Boolean (true/false)                                             │
│  Int     │ 64-bit integer                                                   │
│  Float   │ 64-bit floating point                                            │
│  String  │ Interned string                                                  │
│  Char    │ Single character                                                 │
│  Nil     │ Null value (for nullable types)                                  │
│  Err     │ Error value (for fallible types)                                 │
│  Struct  │ Struct literal (field name → value)                              │
│  Array   │ Array literal (list of values)                                   │
│  Func    │ Function pointer (for const functions)                           │
│  Void    │ No value (void functions)                                        │
│  Unknown │ Unknown value (could not evaluate)                               │
│  Error   │ Evaluation error                                                 │
└─────────────────────────────────────────────────────────────────────────────┘
```

```cpp
ConstantValue Methods:
│
├── // Constructors
│   ConstantValue(bool)              // Bool
│   ConstantValue(int64_t)           // Int
│   ConstantValue(double)            // Float
│   ConstantValue(InternedString)    // String or Char
│   ConstantValue(FuncDeclAST*)      // Func (function pointer)
│   ConstantValue(StructFields)      // Struct
│   ConstantValue(ArrayElements)     // Array
│
├── // Static Factory Methods
│   ConstantValue::nil()             // Nil
│   ConstantValue::err()             // Err
│   ConstantValue::voidValue()       // Void
│   ConstantValue::unknown()         // Unknown
│   ConstantValue::error()           // Error
│
└── // Predicates
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

#### EVALUATION CATEGORIES

```cpp
┌─────────────────────────────────────────────────────────────────────────────┐
│                      Evaluation Categories Summary                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                    Basic Literal Evaluation                           │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  Input          │ Output          │ Notes                             │  │
│  │  true/false     │ Bool            │ Direct conversion                 │  │
│  │  42, 0xFF, 0b10 │ Int             │ std::stoll with base detection    │  │
│  │  3.14           │ Float           │ std::stod                         │  │
│  │  "hello"        │ String          │ Interned string                   │  │
│  │  'a'            │ Char            │ Single character                  │  │
│  │  nil            │ Nil             │ Null value                        │  │
│  │  err            │ Err             │ Error value                       │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                    Identifier Resolution                              │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  Scenario                     │ Result                                │  │
│  │  const X = 42                 │ Returns 42                            │  │
│  │  let X = 42                   │ Unknown (non-const)                   │  │
│  │  X (currently evaluating)     │ Error (circular dependency)           │  │
│  │  X (const function)           │ Func pointer                          │  │
│  │  _ (discard)                  │ Unknown                               │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                    Binary Operations                                  │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  Category         │ Supported Types           │ Notes                 │  │
│  │  Arithmetic       │ Int, Float                │ Overflow checks       │  │
│  │  String Concat    │ String + String           │ Interned result       │  │
│  │  Comparison       │ All comparable types      │ Type mismatch error   │  │
│  │  Logical          │ Bool                      │ Short-circuit         │  │
│  │  Bitwise          │ Int                       │ Shift bounds check    │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                    Control Flow                                       │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  Construct        │ Support      │ Notes                              │  │
│  │  if/else          │ Full         │ Type narrowing, branch selection   │  │
│  │  while loops      │ Full         │ MAX_ITERATIONS limit (10000)       │  │
│  │  for loops        │ Range only   │ Index variable binding             │  │
│  │  switch/case      │ Full         │ Range cases, exhaustiveness        │  │
│  │  return           │ Full         │ Type checking, early exit          │  │
│  │  blocks           │ Full         │ Scoping, local declarations        │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                    Composite Types                                    │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  Type             │ Support      │ Notes                              │  │
│  │  Struct literals  │ Full         │ Default values, field validation   │  │
│  │  Array literals   │ Full         │ Type consistency check             │  │
│  │  Field access     │ Full         │ Struct field lookup                │  │
│  │  Null coalesce    │ Full         │ nil/err fallback                   │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │                    Const Functions                                    │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  Feature          │ Support      │ Notes                              │  │
│  │  Function calls   │ Full         │ Recursion limit (1000)             │  │
│  │  Parameters       │ Full         │ Type binding                       │  │
│  │  Return values    │ Full         │ Type checking                      │  │
│  │  Local variables  │ Const only   │ Mutable vars not allowed           │  │
│  │  Generic functions│ Limited      │ Must provide explicit args         │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

### Intrinsic and Attribute Validation (`registry/`)

The validation of intrinsics and attributes is performed during semantic analysis to ensure correctness before code generation. These validators are pure functions that operate on the AST and report diagnostics via `SemaContext`.

---

#### Intrinsic Validation (`IntrinsicValidator`)

Intrinsic validation checks that intrinsic calls (`#name(...)`) have correct argument counts, argument types, and semantics specific to each intrinsic.

**Purpose:**
- Validates argument counts against intrinsic signatures
- Validates argument types (numeric, pointer, string, etc.)
- Performs semantic validation for specific intrinsics (e.g., `#scope_exit`, atomic ordering)
- Computes return types and value states for intrinsics

```cpp
validateIntrinsicCall(expr, ctx)
│
├── // 1. Look up intrinsic in registry
├── info = IntrinsicRegistry.getInfo(expr->intrinsicName)
├── if (!info) → error "unknown intrinsic"
│
├── // 2. Validate argument count
├── if (!validateIntrinsicArgCount(name, args.size(), ctx))
│   └── error: expects N argument(s), got M
│
├── // 3. Dispatch to specific validator by name category
├── if (name in FLOATING_POINT_OPS)   → validateFloatingPoint(expr, ctx)
├── if (name in MEMORY_OPS)           → validateMemoryOp(expr, ctx)
├── if (name == "fence")              → validateFence(expr, ctx)
├── if (name in STRING_OPS)           → validateStringOp(expr, ctx)
├── if (name in POINTER_OPS)          → validatePointerOp(expr, ctx)
├── if (name == "scope_exit")         → validateScopeExit(expr, ctx)
├── if (name starts with "atomic_")   → validateAtomicOp(expr, ctx)
├── if (name starts with "simd_")     → validateSIMD(expr, ctx)
├── if (name in MEMORY_MGMT_OPS)      → validateMemoryManagement(expr, ctx)
├── if (name in BIT_OPS)              → validateIntArg(args[0], "value", ctx)
├── if (name in PREFETCH_OPS)         → validatePtrArg(args[0], "ptr", ctx)
├── if (name in LIKELY_OPS)           → validateBoolArg(args[0], "condition", ctx)
├── if (name == "pause")              → return true
└── if (name in INSPECTION_OPS)       → return true (minimal validation)
```

**Intrinsic Categories:**

| Category            | Intrinsics                                                                                                                                                 | Validator                    |
| ------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------- |
| Floating-Point Math | `sqrt`, `abs`, `fma`, `ceil`, `floor`, `round`, `pow`, `min`, `max`                                                                                        | `validateFloatingPoint()`    |
| Memory Operations   | `memcpy`, `memmove`, `memset`                                                                                                                              | `validateMemoryOp()`         |
| String Operations   | `str_len`, `str_ptr`, `str_from_ptr`, `str_concat`, `str_eq`, `str_slice`, `str_byte_at`                                                                   | `validateStringOp()`         |
| Pointer Operations  | `addrof`, `toRef`, `toPtr`, `ptrOffset`, `ptrDiff`                                                                                                         | `validatePointerOp()`        |
| Atomic Operations   | `atomic_load`, `atomic_store`, `atomic_add`, `atomic_sub`, `atomic_and`, `atomic_or`, `atomic_xor`, `atomic_cas`                                           | `validateAtomicOp()`         |
| SIMD Operations     | `simd_add`, `simd_sub`, `simd_mul`, `simd_div`, `simd_fma`, `simd_min`, `simd_max`, `simd_splat`, `simd_load`, `simd_store`, `simd_extract`, `simd_insert` | `validateSIMD()`             |
| Memory Management   | `alloc`, `free`, `arena_create`, `arena_alloc`, `arena_reset`, `arena_free`                                                                                | `validateMemoryManagement()` |
| Type Inspection     | `sizeof`, `alignof`, `typeof`, `nameof`, `tostr`, `ptrstr`, `bitcast`                                                                                      | Minimal validation           |
| Control Flow        | `likely`, `unlikely`, `scope_exit`                                                                                                                         | `validateScopeExit()`        |

---

#### `#scope_exit` Validation

The `#scope_exit` intrinsic has the most complex validation because it registers a callback to be executed when the current scope exits.

```cpp
validateScopeExit(expr, ctx)
│
├── // 1. Must be inside a function body
├── if (!ctx.stack.insideFunction())
│   └── error: #scope_exit only valid inside function body
│
├── // 2. Must have at least one argument (the function to call)
├── if (expr->args.empty())
│   └── error: expects at least 1 argument
│
├── // 3. Resolve and validate the callback function
├── funcType = resolveExpr(funcArg)
├── if (!funcType || !funcType->isa<FuncTypeAST>())
│   └── error: expects a function as the first argument
│
├── // 4. Validate generic instantiation
├── if (funcDecl has generic params && no generic args provided)
│   └── error: generic parameters require explicit arguments
├── if (funcDecl has no generic params && generic args provided)
│   └── error: function is not generic
│
├── // 5. The function must have exactly one parameter group (not curried)
├── if (func->isCurried())
│   └── error: curried functions not allowed (use a wrapper closure)
│
├── // 6. The function must return void
├── if (func->returnType)
│   └── error: callback must return void (cannot return during unwinding)
│
├── // 7. No variadic parameters
├── if (any param->isVariadic)
│   └── error: variadic parameters not supported in cleanup callbacks
│
├── // 8. Validate argument count matches function parameters
├── if (callbackArgs != paramCount)
│   └── error: callback expects N argument(s), got M
│
├── // 9. Validate each argument type against callback parameters
├── for each arg:
│   ├── argType = resolveExprWithTarget(arg, expectedType)
│   ├── if (arg->valueState == Nil && !isNullableType(expectedType))
│   │   └── error: cannot pass nil to non-nullable parameter
│   └── if (arg->valueState == Err && !isFallibleType(expectedType))
│       └── error: cannot pass err to non-fallible parameter
│
├── // 10. Check for field access capture issues (warning)
├── if (funcArg is FieldAccessExprAST)
│   └── warning: function reference from struct field may capture lifetime
│
├── // 11. Get the current block and register the exit callback
├── currentBlock = ctx.stack.currentBlock()
├── registration = ctx.arena.make<ScopeExitRegistration>()
├── registration->callExpr = expr
├── registration->callback = funcDecl
├── registration->args = argsBuilder.build()
│
└── // 12. Append to current block's scopeExits
    currentBlock->scopeExits.push_back(registration)
```

**Validation Rules for `#scope_exit`:**

| Rule                     | Description                                                              |
| ------------------------ | ------------------------------------------------------------------------ |
| **Inside Function**      | `#scope_exit` is only valid inside a function body (not at module scope) |
| **Callback is Function** | The first argument must be a function type                               |
| **Not Curried**          | The callback must have exactly one parameter group                       |
| **Returns Void**         | The callback must return void (cannot return during unwinding)           |
| **No Variadic**          | The callback cannot have variadic parameters                             |
| **Arg Count Match**      | The number of arguments must match the callback's parameter count        |
| **Type Match**           | Each argument must be assignable to the corresponding parameter type     |
| **Nil/Err Handling**     | Cannot pass `nil` to non-nullable or `err` to non-fallible parameters    |

---

#### Return Type and Value State

```cpp
getIntrinsicReturnType(expr, targetType, ctx)
│
├── // Void intrinsics
├── if (name in VOID_INTRINSICS) return nullptr
├── if (name == "scope_exit") return nullptr
│
├── // Type/Value Inspection
├── if (name == "sizeof" || name == "alignof") → ctx.getIntType()
├── if (name == "typeof" || name == "nameof" || name == "tostr" || name == "ptrstr")
│   → ctx.getStringType()
├── if (name == "addrof") → PtrTypeAST(inner type)
├── if (name == "toRef") → RefTypeAST(inner type)
├── if (name == "toPtr") → PtrTypeAST(inner type)
├── if (name == "bitcast") → targetType
│
├── // String Operations
├── if (name in STRING_RETURN_OPS) → ctx.getStringType()
├── if (name == "str_ptr") → PtrTypeAST(intType)
├── if (name == "str_eq") → ctx.getBoolType()
├── if (name == "str_byte_at") → ctx.getIntType()
│
├── // Memory Management
├── if (name == "alloc" || name == "arena_alloc")
│   → PtrTypeAST(intType)
├── if (name == "arena_create") → targetType (ArenaDescriptor)
│
└── // SIMD (simd_store returns void)
    if (name == "simd_store") return nullptr
    else return targetType
```

```cpp
getIntrinsicValueState(expr, ctx)
│
├── if (isIntrinsicVoid(name) || name == "scope_exit") → ValueState::None
├── if (name == "alloc" || name == "arena_alloc") → ValueState::Unknown
├── if (name == "toRef") → ValueState::Definite
├── if (name == "fence" || name == "pause") → ValueState::Definite
└── default → ValueState::Definite
```

---

#### Argument Type Validators

The following helpers validate that an argument has a specific type:

```cpp
validatePtrArg(arg, argName, ctx)
├── if (!arg->resolvedType || !arg->resolvedType->isa<PtrTypeAST>())
│   └── error: argument 'name' expects pointer type
└── return true

validateNumericArg(arg, argName, ctx)
├── if (!arg->resolvedType || !isNumericType(arg->resolvedType))
│   └── error: argument 'name' expects numeric type
└── return true

validateIntArg(arg, argName, ctx)
├── if (!arg->resolvedType || !isIntegerType(arg->resolvedType))
│   └── error: argument 'name' expects integer type
└── return true

validateStringArg(arg, argName, ctx)
├── if (!arg->resolvedType || !isStringType(arg->resolvedType))
│   └── error: argument 'name' expects string type
└── return true

validateBoolArg(arg, argName, ctx)
├── if (!arg->resolvedType || !isBoolType(arg->resolvedType))
│   └── error: argument 'name' expects boolean type
└── return true

validateRefArg(arg, argName, ctx)
├── if (!arg->resolvedType || !arg->resolvedType->isa<RefTypeAST>())
│   └── error: argument 'name' expects reference type
└── return true
```

---

#### Attribute Validation (`AttributeValidator`)

Attribute validation checks that attributes (`@[name]`) are correctly applied to declarations.

**Purpose:**
- Validates that attributes are allowed on the declaration kind
- Validates argument counts and types
- Performs semantic validation for specific attributes
- Detects duplicate attributes

```cpp
validateAllAttributes(decl, ctx)
│
├── // Validate each attribute
├── for each attr in decl->attributes:
│   └── if (!validateAttribute(attr, decl, ctx)) allValid = false
│
├── // Check for duplicate attributes
├── for each attr in decl->attributes:
│   if (seen.contains(attr->name))
│       → error: duplicate attribute '@name'
│   seen.insert(attr->name)
│
└── return allValid
```

```cpp
validateAttribute(attr, owner, ctx)
│
├── // 1. Look up attribute in registry
├── info = AttributeRegistry.getInfo(attr->name)
├── if (!info) → error: unknown attribute
│
├── // 2. Check: Is this attribute allowed on this declaration kind?
├── if (!isAllowedOnDecl(attr->name, owner->kind))
│   └── error: attribute cannot be applied to this declaration kind
│
├── // 3. Check: Is this attribute only for generic declarations?
├── if (info->appliesToGenericOnly && !isGeneric(owner))
│   └── error: attribute can only be applied to generic declarations
│
├── // 4. Dispatch to specific validator
├── if (name == "export")     → validateExport(attr, owner, ctx)
├── if (name == "foreign")    → validateForeign(attr, owner, ctx)
├── if (name == "link")       → validateLink(attr, owner, ctx)
├── if (name == "deprecated") → validateDeprecated(attr, owner, ctx)
├── if (name == "inline" || name == "noinline")
│   → validateInlineHint(attr, owner, ctx)
├── if (name == "specialize") → validateSpecialize(attr, owner, ctx)
│
└── // Generic validation
    ├── validateArgCount(attr, info->minArgs, info->maxArgs, ctx)
    └── if (info->requiresStringArgs)
        └── for each arg: validateStringArg(arg, ctx)
```

---

#### Individual Attribute Validators

```cpp
validateExport(attr, owner, ctx)
│
├── // 1. No arguments
├── if (!attr->args.empty())
│   └── error: '@[export]' takes no arguments
│
└── // 2. Only at module level
    if (!isModuleLevelDeclaration(owner, ctx))
        └── error: '@[export]' is only legal at module level
```

```cpp
validateForeign(attr, owner, ctx)
│
├── // 1. Only on functions
├── if (!owner->isa<FuncDeclAST>())
│   └── error: '@[foreign]' is only legal on functions
│
├── // 2. Exactly one argument
├── if (!validateArgCount(attr, 1, 1, ctx)) return false
│
├── // 3. Validate ABI string
├── if (!validateStringArg(attr->args[0], "ABI", ctx)) return false
│
├── // 4. Must be "C"
├── abi = ctx.pool.lookup(lit->value)
├── if (abi != "C")
│   └── error: only "C" ABI is supported
│
└── // 5. Warn if function has a body
    if (func->body)
        └── warning: foreign function has a body (will be ignored)
```

```cpp
validateLink(attr, owner, ctx)
│
├── // 1. Only at module level or on functions
├── if (!isModuleLevelDeclaration(owner, ctx) && !owner->isa<FuncDeclAST>())
│   └── error: '@[link]' is only legal at module level or on functions
│
├── // 2. At least one argument
├── if (attr->args.empty())
│   └── error: expects at least 1 argument (library name or file path)
│
├── // 3. Validate each argument is a non-empty string literal
├── for each arg:
│   ├── if (!arg is LiteralExpr of String/RawString)
│   │   └── error: must be a string literal
│   ├── if (value.empty())
│   │   └── error: cannot be an empty string
│   └── // Warnings about common mistakes
│       ├── if (value contains space) → warning: library names should not contain spaces
│       ├── if (value starts with "./") → warning: prefer absolute paths
│       └── if (value has no extension) → warning: prefer '.lib' or '.dll' on Windows
│
└── return allValid
```

```cpp
validateDeprecated(attr, owner, ctx)
│
├── // 1. At most one argument
├── if (attr->args.size() > 1)
│   └── error: expects at most 1 argument (the message)
│
└── // 2. Validate optional message argument
    if (!attr->args.empty() && !validateStringArg(attr->args[0], "message", ctx))
        └── return false
```

```cpp
validateInlineHint(attr, owner, ctx)
│
├── // 1. Only on functions
├── if (!owner->isa<FuncDeclAST>())
│   └── error: '@[inline]'/'@[noinline]' is only legal on functions
│
├── // 2. No arguments
├── if (!attr->args.empty())
│   └── error: takes no arguments
│
├── // 3. Warn on foreign functions
├── if (owner has @[foreign] attribute)
│   └── warning: foreign function cannot be inlined
│
└── // 4. Store flag on function
    func->isInline = (name == "inline")
    func->isNoInline = (name == "noinline")
```

```cpp
validateSpecialize(attr, owner, ctx)
│
├── // 1. Only on generic declarations
├── if (!isGeneric(owner))
│   └── error: can only be applied to generic declarations
│
├── // 2. No arguments
├── if (!attr->args.empty())
│   └── error: takes no arguments
│
└── // 3. Mark as needing specialization
    if (owner->isa<FuncDeclAST>())
        func->shouldSpecialize = true
    if (owner->isa<StructDeclAST>())
        struct->shouldSpecialize = true
```

---

#### Attribute Registry Information

The attribute registry defines which declarations each attribute can attach to:

| Attribute       | Allowed Declaration Kinds                                    | Applies to Generic Only | Min Args | Max Args | Requires String Args |
| --------------- | ------------------------------------------------------------ | ----------------------- | -------- | -------- | -------------------- |
| `@[export]`     | `VarDecl`, `FuncDecl`, `StructDecl`, `EnumDecl`, `TraitDecl` | No                      | 0        | 0        | No                   |
| `@[foreign]`    | `FuncDecl`                                                   | No                      | 1        | 1        | Yes                  |
| `@[link]`       | `VarDecl`, `FuncDecl`, `StructDecl`, `EnumDecl`, `TraitDecl` | No                      | 1        | 0        | Yes                  |
| `@[deprecated]` | All declarations                                             | No                      | 0        | 1        | Yes                  |
| `@[inline]`     | `FuncDecl`                                                   | No                      | 0        | 0        | No                   |
| `@[noinline]`   | `FuncDecl`                                                   | No                      | 0        | 0        | No                   |
| `@[specialize]` | `FuncDecl`, `StructDecl`                                     | Yes                     | 0        | 0        | No                   |

---

#### Helper Functions

```cpp
// ─── Argument Type Validators ─────────────────────────────────────────────
validateStringArg(arg, argName, ctx)
validateArgCount(attr, min, max, ctx)
isModuleLevelDeclaration(decl, ctx)
supportsAttributes(decl)

// ─── Duplicate Detection ──────────────────────────────────────────────────
// AttributeValidator checks for duplicate attributes by name on the same
// declaration and reports an error if found.
```

---

#### File Structure

```
src/sema/registry/
├── AttributeValidator.hpp         # Public API for attribute validation
├── AttributeValidator.cpp         # Implementation
├── IntrinsicValidator.hpp         # Public API for intrinsic validation
└── IntrinsicValidator.cpp         # Implementation
```

---

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

---
