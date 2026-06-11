# Deprecated, do not reference this file

# Semantic Structure

The Luc semantic analyzer runs a multi-phase pipeline over parsed ASTs. `SemanticAnalyzer` owns the symbol table, context, type dispatcher, and collector; all checking functions are declared in `checkers/SemanticChecker.hpp` and implemented in the files below.

## File layout

```
src/semantic/
├── SemanticAnalyzer.hpp                # Orchestrator: analyze(), phase methods, CompilationMode
├── SemanticAnalyzer.cpp                # Phase 0–4 pipeline, trait map, entry-point validation
├── SemanticSymbol.hpp                  # Symbol, SymbolKind, SymbolUtils
├── SymbolTable.hpp                     # Scope stack, declare/lookup
├── SymbolTable.cpp
├── Annotator.cpp                       # Phase 4: ASTVisitor, stamps isConst / isBehaviorMember
│
├── helpers/
│   ├── SemanticContext.hpp             # Shared state: pool, arena, symbols, dispatcher, typeTraits
│   ├── SemanticContext.cpp             # implementsTrait()
│   ├── NameMangler.hpp                 # mangleType, mangleFunc, mangleMethod (canonical type keys)
│   └── ResolverHelpers.hpp             # isStructType, isEnumType, type classification helpers
│
├── collectors/                         # Phase 1: symbol collection
│   ├── SemanticCollector.hpp           # collectProgram, per-decl collect* methods
│   └── SemanticCollector.cpp
│
├── resolveType/                        # Phase 2: type annotation resolution
│   ├── TypeDispatcher.hpp              # Main entry point; owns all resolvers
│   ├── TypeDispatcher.cpp              # resolveType dispatch + decl-level forwarding
│   │
│   ├── core/
│   │   ├── GenericParamHandler.hpp       # Generic param + substitution stacks
│   │   ├── GenericParamHandler.cpp
│   │   ├── TypeCloner.hpp              # Deep clone of type AST nodes
│   │   ├── TypeCloner.cpp
│   │   ├── ConstraintChecker.hpp       # namespace ConstraintChecker: satisfies, type queries
│   │   └── ConstraintChecker.cpp
│   │
│   ├── primitive/
│   │   └── PrimitiveResolver.hpp         # Primitive type resolution (header-only)
│   │
│   ├── named/
│   │   ├── NamedResolver.hpp
│   │   └── NamedResolver.cpp           # User-defined / generic named types
│   │
│   ├── composite/
│   │   ├── NullableResolver.hpp
│   │   ├── NullableResolver.cpp
│   │   ├── ResultResolver.hpp          # T!E nesting validation (header-only)
│   │   ├── ArrayResolver.hpp           # [_, T], [*, T], [N, T] (header-only)
│   │   ├── RefResolver.hpp             # &T (header-only)
│   │   ├── PtrResolver.hpp             # *T (header-only)
│   │   ├── FuncResolver.hpp
│   │   └── FuncResolver.cpp            # Function types (qualifiers, params, returns)
│   │
│   ├── decl/
│   │   ├── TypeAliasResolver.hpp       # Generic alias RHS (header-only)
│   │   ├── StructResolver.hpp
│   │   ├── StructResolver.cpp          # Field types, self-type
│   │   ├── FuncSignatureResolver.hpp   # Function signature (header-only)
│   │   ├── ImplResolver.hpp
│   │   ├── ImplResolver.cpp            # Impl target, generics, method resolution
│   │   ├── FromResolver.hpp
│   │   ├── FromResolver.cpp            # From entry resolution (inline and path)
│   │   └── VarResolver.hpp             # Variable declared type (header-only)
│   │
│   ├── injection/
│   │   ├── InjectionTransformer.hpp
│   │   └── InjectionTransformer.cpp    # Injection form `!` type transformation
│   │
│   └── callable/
│       ├── CallableExtractor.hpp
│       └── CallableExtractor.cpp       # Function reference extraction from callables
│
├── checkType/                          # Type compatibility (Phase 3 helpers)
│   ├── TypeChecker.hpp                 # namespace TypeChecker: isEqual, isAssignable, unify, …
│   └── TypeChecker.cpp
│
└── checkers/                           # Phase 3: semantic checking
    ├── SemanticChecker.hpp             # Unified interface: all check* declarations
    │
    ├── declCheckers/
    │   ├── DeclDispatcher.cpp          # checkTopLevelDecl
    │   ├── DeclHelpers.hpp             # checkAttributes, isConstExpr, impl helpers
    │   ├── checkVarDecl.cpp            # checkVarDecl
    │   ├── checkFuncDecl.cpp           # checkFuncDecl
    │   ├── checkStructDecl.cpp         # checkStructDecl
    │   ├── checkEnumDecl.cpp           # checkEnumDecl
    │   ├── checkTraitDecl.cpp          # checkTraitDecl
    │   ├── checkImplDecl.cpp           # checkImplDecl
    │   └── checkFromDecl.cpp           # checkFromDecl
    │
    ├── stmtCheckers/
    │   ├── StmtDispatcher.cpp          # checkStmt
    │   ├── BlockChecker.cpp            # checkBlockStmt
    │   ├── DeclStmtChecker.cpp         # checkDeclStmt (local decl dispatch)
    │   ├── ExprStmtChecker.cpp         # checkExprStmt
    │   ├── ControlFlowChecker.cpp      # checkIfStmt, checkReturnStmt, checkBreakStmt, checkContinueStmt
    │   ├── LoopChecker.cpp             # checkWhileStmt, checkForStmt, checkDoWhileStmt
    │   ├── SwitchChecker.cpp           # checkSwitchStmt
    │   ├── MultiVarDeclChecker.cpp     # checkMultiVarDecl
    │   └── MultiAssignChecker.cpp      # checkMultiAssignStmt
    │
    └── exprCheckers/
        ├── ExprDispatcher.cpp          # checkExpr (caches resolvedType on node)
        │
        ├── literal/
        │   ├── LiteralChecker.cpp      # checkLiteralExpr
        │   ├── ArrayLiteralChecker.cpp # checkArrayLiteralExpr
        │   ├── StructLiteralChecker.cpp# checkStructLiteralExpr
        │   └── AnonFuncChecker.cpp     # checkAnonFuncExpr
        │
        ├── operator/
        │   ├── BinaryChecker.cpp       # checkBinaryExpr, checkUnaryExpr
        │   ├── AssignChecker.cpp       # checkAssignExpr
        │   ├── IsChecker.cpp           # checkIsExpr
        │   ├── NullCoalesceChecker.cpp # checkNullCoalesceExpr
        │   ├── PipelineChecker.cpp     # checkPipelineExpr
        │   └── ComposeChecker.cpp      # checkComposeExpr
        │
        ├── special/
        │   ├── AwaitChecker.cpp        # checkAwaitExpr
        │   ├── IfExprChecker.cpp       # checkIfExpr
        │   ├── IntrinsicChecker.cpp    # checkIntrinsicCallExpr
        │   ├── RangeChecker.cpp        # checkRangeExpr
        │   ├── TypeConvChecker.cpp     # checkTypeConvExpr
        │   └── NullableChainChecker.cpp# checkNullableChainExpr
        │
        ├── other/
        │   ├── CallChecker.cpp         # checkCallExpr
        │   ├── IndexChecker.cpp        # checkIndexExpr
        │   ├── FieldAccessChecker.cpp  # checkFieldAccessExpr
        │   ├── BehaviorAccessChecker.cpp# checkBehaviorAccessExpr
        │   └── IdentifierChecker.cpp   # checkIdentifierExpr
        │
        └── match/
            ├── MatchChecker.cpp        # checkMatchExpr
            └── PatternChecker.cpp      # checkPattern (per-pattern validation)
```

## Analysis pipeline

Entry point: `SemanticAnalyzer::analyze(files)` in `SemanticAnalyzer.cpp`.

```
analyze
├── Phase 0: resolveImports              [SemanticAnalyzer.cpp]
│   └── per file: duplicate `use` path detection
├── Phase 1: collectSymbols                [collectors/SemanticCollector.cpp]
│   └── collectProgram → collect* per decl kind
├── Phase 1.5: validateNoDuplicateSymbols  [SemanticAnalyzer.cpp]
│   └── cross-file duplicate symbol names in global scope
├── Phase 2: resolveTypes                  [resolveType/TypeDispatcher.cpp]
│   └── per top-level decl: resolveTypeAlias | resolveStructFields |
│       resolveFunctionSignature | resolveImplMethods | resolveFromEntries | resolveVarType
├── Phase 2.5: buildTraitConformanceMap    [SemanticAnalyzer.cpp]
│   └── scan impl symbols → ctx.typeTraits (mangled type key → trait names)
├── Phase 3: checkDecls                    [declCheckers/DeclDispatcher.cpp]
│   └── checkTopLevelDecl per declaration
├── Phase 3.5: validateEntryPoint          [SemanticAnalyzer.cpp]
│   └── `main` signature, export/const, @aot/@jit → CompilationMode
└── Phase 4: annotate                      [Annotator.cpp]
    └── annotateAll → Annotator ASTVisitor (isConst, isBehaviorMember)
```

`SemanticContext` is passed by reference through every phase. After construction, `ctx_.dispatcher` is wired to the owned `TypeDispatcher` so checkers can resolve types during Phase 3.

## Phase 1: Symbol collection (`SemanticCollector`)

```
collectProgram                               [collectors/SemanticCollector.cpp]
└── switch on decl kind
    ├── UseDecl        → collectUseDecl
    ├── VarDecl        → collectVarDecl
    ├── FuncDecl       → collectFuncDecl
    ├── StructDecl     → collectStructDecl
    ├── EnumDecl       → collectEnumDecl     (+ mangled enum variant symbols)
    ├── TraitDecl      → collectTraitDecl    (+ mangled trait method symbols)
    ├── ImplDecl       → collectImplDecl     (unique `__impl_N` symbol; no trait map yet)
    ├── FromDecl       → collectFromDecl
    └── TypeAliasDecl  → collectTypeAliasDecl
```

Each `collect*` builds a `Symbol` and calls `declareSymbol` (local duplicate check via `lookupLocal`). `@extern` metadata is extracted in `extractExternMetadata`. Trait conformance is deferred to Phase 2.5.

## Phase 2: Type resolution (`TypeDispatcher`)

`TypeDispatcher` owns all specialized resolvers as `unique_ptr` members. Construction order matters: `NullableResolver` receives `FuncResolver` after both are built; `ImplResolver` and `FromResolver` depend on `InjectionTransformer` and `CallableExtractor`.

### Resolver dependency graph

```
                    ┌─────────────────┐
                    │ TypeDispatcher  │
                    └────────┬────────┘
                             │
        ┌────────────────────┼────────────────────┐
        │                    │                    │
        ▼                    ▼                    ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│GenericParam   │    │  TypeCloner   │    │Constraint     │
│Handler        │    │               │    │Checker        │
└───────────────┘    └───────────────┘    └───────────────┘
        │                    │                    │
        └────────────────────┼────────────────────┘
                             │
        ┌────────────────────┼────────────────────┬────────────────────┐
        │                    │                    │                    │
        ▼                    ▼                    ▼                    ▼
┌───────────────┐    ┌───────────────┐    ┌────────────────┐   ┌───────────────┐
│NamedResolver  │    │ FuncResolver  │    │NullableResolver│   │ArrayResolver  │
└───────────────┘    └───────────────┘    └────────────────┘   └───────────────┘
        │                    │                    │
        └────────────────────┼────────────────────┘
                             │
        ┌────────────────────┼────────────────────┬────────────────────┐
        │                    │                    │                    │
        ▼                    ▼                    ▼                    ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│Callable       │    │Injection      │    │ ImplResolver  │    │ FromResolver  │
│Extractor      │    │Transformer    │    │               │    │               │
└───────────────┘    └───────────────┘    └───────────────┘    └───────────────┘
```

### Declaration-level (`SemanticAnalyzer::resolveTypes`)

```
resolveTypes                                 [SemanticAnalyzer.cpp]
└── per top-level decl
    ├── TypeAliasDecl  → dispatcher.resolveTypeAlias      → TypeAliasResolver
    ├── StructDecl     → dispatcher.resolveStructFields   → StructResolver
    ├── FuncDecl       → dispatcher.resolveFunctionSignature → FuncSignatureResolver
    ├── ImplDecl       → dispatcher.resolveImplMethods    → ImplResolver
    ├── FromDecl       → dispatcher.resolveFromEntries    → FromResolver
    └── VarDecl        → dispatcher.resolveVarType        → VarResolver
```

### Type node dispatch (`TypeDispatcher::resolveType`)

```
resolveType                                  [resolveType/TypeDispatcher.cpp]
└── switch on type kind
    ├── PrimitiveType  → PrimitiveResolver::resolve
    ├── NamedType      → NamedResolver::resolve
    ├── NullableType   → NullableResolver::resolve
    ├── ResultType     → ResultResolver::resolve
    ├── ArrayType      → ArrayResolver::resolve
    ├── RefType        → RefResolver::resolve
    ├── PtrType        → PtrResolver::resolve
    └── FuncType       → FuncResolver::resolve
```

### Component responsibilities

| Component               | Responsibility                                             | Location                 |
| ----------------------- | ---------------------------------------------------------- | ------------------------ |
| `GenericParamHandler`   | Generic parameter stack, substitution maps                 | `resolveType/core/`      |
| `TypeCloner`            | Deep cloning of type AST nodes                             | `resolveType/core/`      |
| `ConstraintChecker`     | Trait constraint satisfaction, type classification         | `resolveType/core/`      |
| `PrimitiveResolver`     | Primitive type resolution                                  | `resolveType/primitive/` |
| `NamedResolver`         | User-defined and generic named type resolution             | `resolveType/named/`     |
| `NullableResolver`      | `T?` resolution (depends on `FuncResolver`)                | `resolveType/composite/` |
| `ResultResolver`        | `T!E` resolution with nesting validation                   | `resolveType/composite/` |
| `ArrayResolver`         | `[_, T]`, `[*, T]`, `[N, T]` resolution                    | `resolveType/composite/` |
| `RefResolver`           | `&T` reference resolution                                  | `resolveType/composite/` |
| `PtrResolver`           | `*T` pointer resolution                                    | `resolveType/composite/` |
| `FuncResolver`          | Function type resolution (qualifiers, parameters, returns) | `resolveType/composite/` |
| `TypeAliasResolver`     | Type alias generic scope setup                             | `resolveType/decl/`      |
| `StructResolver`        | Struct field types, self-type creation                     | `resolveType/decl/`      |
| `FuncSignatureResolver` | Function signature generic scope setup                     | `resolveType/decl/`      |
| `ImplResolver`          | Impl target, generic arity, method resolution              | `resolveType/decl/`      |
| `FromResolver`          | From entry resolution (inline and path)                    | `resolveType/decl/`      |
| `VarResolver`           | Variable declared type binding on symbol                   | `resolveType/decl/`      |
| `InjectionTransformer`  | Injection form `!` type transformation                     | `resolveType/injection/` |
| `CallableExtractor`     | Function reference extraction from callable expressions    | `resolveType/callable/`  |
| `ResolverHelpers`       | Type classification helpers used by resolvers              | `helpers/`               |

### Generic parameter and substitution management

Stacks live in `GenericParamHandler`; `TypeDispatcher` forwards to them:

```cpp
// Via TypeDispatcher (or ctx.dispatcher in checkers)
pushGenericParams / popGenericParams
pushSubstitutionMap / popSubstitutionMap
lookupSubstitution / isGenericParam
```

### Phase 2.5: Trait conformance map

```
buildTraitConformanceMap                     [SemanticAnalyzer.cpp]
└── for each SymbolKind::Impl in global scope
    ├── skip impls without traitRef
    ├── resolve target type via dispatcher.resolveType
    ├── unwrap type aliases
    ├── key = NameMangler::mangleType(unwrapped, pool, symbols)
    └── ctx.typeTraits[key].push_back(traitName)
```

`ConstraintChecker::satisfies(ctx, type, requiredTraits)` and `SemanticContext::implementsTrait` both look up `ctx.typeTraits` using the same mangled type key. The map is built after all type resolution completes and is read-only during Phase 3.

### Type compatibility (stateless helpers)

`namespace TypeChecker` in `checkType/TypeChecker.hpp` — used throughout Phase 3 checkers:

| Function                                      | Purpose                          |
| --------------------------------------------- | -------------------------------- |
| `isEqual` / `isAssignable` / `unify`          | Type compatibility and inference |
| `isCallable`                                  | Call-target validation           |
| `isFromCastable`                              | Custom `from` conversion lookup  |
| `isIntegerType` / `getConstantIntValue`       | Array index and slice bounds     |
| `isValueComparable` / `isReferenceComparable` | Switch / equality rules          |

## Phase 3: Checking

Phase 3 walks top-level declarations, then recursively checks bodies via `checkStmt` and `checkExpr`. Expression checkers return `TypeAST*` and cache it on `node->resolvedType` in `ExprDispatcher.cpp`. Late type resolution uses `ctx.dispatcher->resolveType`.

### Top-level declaration dispatch (`checkTopLevelDecl`)

```
checkTopLevelDecl                            [declCheckers/DeclDispatcher.cpp]
├── VarDecl    → checkVarDecl(ctx, isLocal=false)
├── FuncDecl   → checkFuncDecl
├── StructDecl → checkStructDecl
├── EnumDecl   → checkEnumDecl
├── TraitDecl  → checkTraitDecl
├── ImplDecl   → checkImplDecl
└── FromDecl   → checkFromDecl
```

`PackageDecl` and `UseDecl` are not checked here (imports handled in Phase 0; package is parser-only preamble).

Local declarations inside blocks go through `checkDeclStmt` with `isLocal=true` for `VarDecl` / `FuncDecl`, or `checkTopLevelDecl` for other decl kinds.

### Function / method body flow

```
checkFuncDecl                                [declCheckers/checkFuncDecl.cpp]
├── checkAttributes                          [declCheckers/DeclHelpers.hpp]
├── @extern → metadata on symbol, return (no body)
├── pushGenericParams on dispatcher (if generic)
├── pushScope, declare parameters
└── checkStmt(body, expectedReturn)          [stmtCheckers/StmtDispatcher.cpp]

checkImplDecl                                [declCheckers/checkImplDecl.cpp]
├── pushSubstitutionMap (if generic impl)
└── checkImplMethod per method               [DeclHelpers.hpp]
    ├── injectReceiverSymbol
    ├── declare method parameters
    └── checkStmt(method.body, expectedReturn)
```

### Statement dispatch (`checkStmt`)

```
checkStmt                                    [stmtCheckers/StmtDispatcher.cpp]
├── BlockStmt        → checkBlockStmt        → pushScope, checkStmt* per stmt
├── ExprStmt         → checkExprStmt         → checkExpr
├── DeclStmt         → checkDeclStmt         → checkVarDecl | checkFuncDecl (local) |
│                                              checkTopLevelDecl (other)
├── IfStmt           → checkIfStmt
├── WhileStmt        → checkWhileStmt
├── ForStmt          → checkForStmt
├── DoWhileStmt      → checkDoWhileStmt
├── ReturnStmt       → checkReturnStmt       (validates against expectedReturn)
├── BreakStmt        → checkBreakStmt        (requires loopDepth > 0)
├── ContinueStmt     → checkContinueStmt
├── SwitchStmt       → checkSwitchStmt
├── MultiVarDecl     → checkMultiVarDecl
└── MultiAssignStmt  → checkMultiAssignStmt
```

`expectedReturn` is the function's first return type (or `nullptr` for void). Control-flow and loop checkers pass it through to nested `checkBlockStmt` / `checkStmt` calls.

### Expression dispatch (`checkExpr`)

```
checkExpr                                    [exprCheckers/ExprDispatcher.cpp]
├── if resolvedType already set → return cached
└── switch on expr kind
    ├── LiteralExpr         → checkLiteralExpr
    ├── ArrayLiteralExpr    → checkArrayLiteralExpr
    ├── StructLiteralExpr   → checkStructLiteralExpr
    ├── AnonFuncExpr        → checkAnonFuncExpr
    ├── BinaryExpr          → checkBinaryExpr
    ├── UnaryExpr           → checkUnaryExpr
    ├── AssignExpr          → checkAssignExpr
    ├── IsExpr              → checkIsExpr
    ├── NullCoalesceExpr    → checkNullCoalesceExpr
    ├── PipelineExpr        → checkPipelineExpr
    ├── ComposeExpr         → checkComposeExpr
    ├── AwaitExpr           → checkAwaitExpr
    ├── IfExpr              → checkIfExpr
    ├── IntrinsicCallExpr   → checkIntrinsicCallExpr
    ├── RangeExpr           → checkRangeExpr
    ├── TypeConvExpr        → checkTypeConvExpr
    ├── NullableChainExpr   → checkNullableChainExpr
    ├── CallExpr            → checkCallExpr
    ├── IndexExpr           → checkIndexExpr
    ├── FieldAccessExpr     → checkFieldAccessExpr
    ├── BehaviorAccessExpr  → checkBehaviorAccessExpr
    ├── IdentifierExpr      → checkIdentifierExpr
    └── MatchExpr           → checkMatchExpr
```

Postfix forms (`CallExpr`, `IndexExpr`, `FieldAccessExpr`, etc.) are separate AST nodes; the parser builds them as primary/postfix chains, and each gets its own checker.

### Match / pattern checking

```
checkMatchExpr                               [exprCheckers/match/MatchChecker.cpp]
├── checkExpr(subject)
├── require default arm
└── per arm: pattern binding + checkExpr(body) → unify arm types

checkPattern                                 [exprCheckers/match/PatternChecker.cpp]
├── BindPattern      → accept (introduces binding in match scope)
├── WildcardPattern  → accept
├── LiteralPattern   → compare to subject type
├── TypePattern      → resolve check type, validate against subject
└── StructPattern    → field patterns (recursive checkPattern)
```

## Phase 4: Annotation (`Annotator`)

`annotateAll` in `Annotator.cpp` runs a post-order `ASTVisitor` over all programs. It sets `isConst` on expressions and declarations (const propagation from literals, `const` bindings, and composite expressions). `isBehaviorMember` is reaffirmed on `BehaviorAccessExprAST`.

Fields written during Phase 3 (not Phase 4):

| Field          | Set by                               |
| -------------- | ------------------------------------ |
| `resolvedType` | `checkExpr` (`ExprDispatcher.cpp`)   |
| `scopeDepth`   | block / scope push sites in checkers |

## Shared helpers (called from many paths)

```
checkAttributes        → AttributeRegistry validation, @extern metadata   [DeclHelpers.hpp]
isConstExpr            → compile-time constant detection                   [DeclHelpers.hpp]
injectReceiverSymbol   → impl `self` / receiver alias                    [DeclHelpers.hpp]
checkImplMethod        → impl method scope + body                        [DeclHelpers.hpp]
NameMangler::mangleType / mangleFunc / mangleMethod                       [helpers/NameMangler.hpp]
ResolverHelpers        → isStructType, isEnumType, type queries          [helpers/ResolverHelpers.hpp]
ctx.implementsTrait    → trait lookup via ctx.typeTraits                 [helpers/SemanticContext.cpp]
ctx.enterLoop / exitLoop / enterParallel / exitParallel                    [helpers/SemanticContext.hpp]
ctx.error / ctx.warning / ctx.note                                         [helpers/SemanticContext.hpp]
```

## Component ownership

`SemanticAnalyzer` constructs components in dependency order:

```
SymbolTable symbols_
SemanticContext ctx_(pool, arena, &symbols_)   // ctx before dispatcher
TypeDispatcher dispatcher_(ctx_)
SemanticCollector collector_

// In constructor:
ctx_.dispatcher = &dispatcher_;
```

`ctx.dispatcher` is non-owning; checkers call `ctx.dispatcher->resolveType`, `pushGenericParams`, and `pushSubstitutionMap` for late or local type resolution inside Phase 3.
