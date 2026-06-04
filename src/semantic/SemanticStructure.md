# Semantic Structure

The Luc semantic analyzer runs a multi-phase pipeline over parsed ASTs. `SemanticAnalyzer` owns the symbol table, context, type dispatcher, and collector; all checking functions are declared in `checkers/SemanticChecker.hpp` and implemented in the files below.

## File layout

```
src/semantic/
├── SemanticAnalyzer.hpp                # Orchestrator: analyze(), phase methods, CompilationMode
├── SemanticAnalyzer.cpp                # Phase 0–4 pipeline, entry-point validation, duplicate-symbol check
├── SemanticSymbol.hpp                  # Symbol, SymbolKind, SymbolUtils
├── SymbolTable.hpp                     # Scope stack, declare/lookup
├── SymbolTable.cpp
├── Annotator.cpp                       # Phase 4: ASTVisitor, stamps isConst / isBehaviorMember
│
├── helpers/
│   ├── SemanticContext.hpp             # Shared state: pool, arena, symbols, dispatcher, depth flags, diagnostics
│   └── NameMangler.hpp                 # mangleType, mangleFunc (linker / trait names)
│
├── collectors/                         # Phase 1: symbol collection
│   ├── SemanticCollector.hpp           # collectProgram, per-decl collect* methods
│   └── SemanticCollector.cpp
│
├── resolveType/                        # Phase 2: type annotation resolution
│   │
│   ├── TypeDispatcher.hpp              # Main entry point (renamed from TypeResolver)
│   ├── TypeDispatcher.cpp              # resolveType dispatch only
│   │
│   ├── core/                           # Shared foundational components
│   │   ├── GenericParamHandler.hpp
│   │   ├── GenericParamHandler.cpp
│   │   ├── TypeCloner.hpp
│   │   ├── TypeCloner.cpp
│   │   ├── ConstraintChecker.hpp
│   │   └── ConstraintChecker.cpp
│   │
│   ├── primitive/                      # Primitive type (trivial)
│   │   └── PrimitiveResolver.hpp
│   │
│   ├── named/                          # Named type (complex)
│   │   ├── NamedResolver.hpp
│   │   └── NamedResolver.cpp
│   │
│   ├── composite/                      # Composite type resolvers
│   │   ├── NullableResolver.hpp
│   │   ├── NullableResolver.cpp
│   │   ├── ResultResolver.hpp          # Inline (simple)
│   │   ├── ArrayResolver.hpp           # Inline (simple)
│   │   ├── RefResolver.hpp             # Inline (simple)
│   │   ├── PtrResolver.hpp             # Inline (simple)
│   │   ├── FuncResolver.hpp
│   │   └── FuncResolver.cpp
│   │
│   ├── decl/                           # Declaration-level resolvers
│   │   ├── TypeAliasResolver.hpp       # Inline (simple)
│   │   ├── StructResolver.hpp
│   │   ├── StructResolver.cpp
│   │   ├── FuncSignatureResolver.hpp   # Inline (simple)
│   │   ├── ImplResolver.hpp
│   │   ├── ImplResolver.cpp
│   │   ├── FromResolver.hpp
│   │   ├── FromResolver.cpp
│   │   └── VarResolver.hpp             # Inline (simple)
│   │
│   ├── injection/                      # Injection form transformation
│   │   ├── InjectionTransformer.hpp
│   │   └── InjectionTransformer.cpp
│   │
│   ├── callable/                       # Function reference extraction
│   │   ├── CallableExtractor.hpp
│   │   └── CallableExtractor.cpp
│   │
│   └── helpers/                        # Shared utilities
│       └── ResolverHelpers.hpp
│
├── checkType/                          # Type compatibility (moved from resolveType)
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
            └── PatternChecker.cpp      # checkPattern (static, used from match arms)
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
├── Phase 2: resolveTypes                  [TypeDispatcher.cpp]
│   └── per top-level decl: resolveTypeAlias | resolveStructFields |
│       resolveFunctionSignature | resolveImplMethods | resolveFromEntries | resolveVarType
├── Phase 3: checkDecls                    [declCheckers/DeclDispatcher.cpp]
│   └── checkTopLevelDecl per declaration
├── Phase 3.5: validateEntryPoint          [SemanticAnalyzer.cpp]
│   └── `main` signature, export/const, @aot/@jit → CompilationMode
└── Phase 4: annotate                      [Annotator.cpp]
    └── annotateAll → Annotator ASTVisitor (isConst, isBehaviorMember)
```

`SemanticContext` is passed by reference through every phase. `TypeDispatcher` receives the struct→trait map from `SemanticCollector::getStructTraits()` after Phase 1.

## Phase 1: Symbol collection (`SemanticCollector`)

```
collectProgram                               [collectors/SemanticCollector.cpp]
└── switch on decl kind
    ├── UseDecl        → collectUseDecl
    ├── VarDecl        → collectVarDecl
    ├── FuncDecl       → collectFuncDecl
    ├── StructDecl     → collectStructDecl
    ├── EnumDecl       → collectEnumDecl
    ├── TraitDecl      → collectTraitDecl
    ├── ImplDecl       → collectImplDecl      (records structTraits_)
    ├── FromDecl       → collectFromDecl
    └── TypeAliasDecl  → collectTypeAliasDecl
```

Each `collect*` builds a `Symbol` and calls `declareSymbol` (local duplicate check via `lookupLocal`). `@extern` metadata is extracted in `extractExternMetadata`.

### Dependency Flow Diagram

## Phase 2: Type resolution (`TypeDispatcher`)

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
│Handler        │◄───│               │    │Checker        │
└───────────────┘    └───────────────┘    └───────────────┘
        │                    │                    │
        └────────────────────┼────────────────────┘
                             │
        ┌────────────────────┼────────────────────┬────────────────────┐
        │                    │                    │                    │
        ▼                    ▼                    ▼                    ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│NamedResolver  │    │ FuncResolver  │    │NullableResolver│   │ArrayResolver  │
└───────────────┘    └───────────────┘    └───────────────┘    └───────────────┘
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
    ├── TypeAliasDecl  → resolveTypeAlias
    ├── StructDecl     → resolveStructFields
    ├── FuncDecl       → resolveFunctionSignature
    ├── ImplDecl       → resolveImplMethods
    ├── FromDecl       → resolveFromEntries
    └── VarDecl        → resolveVarType
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

### Component Architecture

The type resolver is decomposed into focused components:

| Component               | Responsibility                                             | Location                 |
| ----------------------- | ---------------------------------------------------------- | ------------------------ |
| `GenericParamHandler`   | Generic parameter stack, substitution maps                 | `resolveType/core/`      |
| `TypeCloner`            | Deep cloning of type AST nodes                             | `resolveType/core/`      |
| `ConstraintChecker`     | Trait constraint satisfaction                              | `resolveType/core/`      |
| `PrimitiveResolver`     | Primitive type resolution (trivial)                        | `resolveType/primitive/` |
| `NamedResolver`         | User-defined type resolution (complex)                     | `resolveType/named/`     |
| `NullableResolver`      | `T?` resolution with validation                            | `resolveType/composite/` |
| `ResultResolver`        | `T!E` resolution with nesting validation                   | `resolveType/composite/` |
| `ArrayResolver`         | `[_, T]`, `[*, T]`, `[N, T]` resolution                    | `resolveType/composite/` |
| `RefResolver`           | `&T` reference resolution                                  | `resolveType/composite/` |
| `PtrResolver`           | `*T` pointer resolution                                    | `resolveType/composite/` |
| `FuncResolver`          | Function type resolution (qualifiers, parameters, returns) | `resolveType/composite/` |
| `TypeAliasResolver`     | Type alias RHS resolution                                  | `resolveType/decl/`      |
| `StructResolver`        | Struct field type resolution, self-type creation           | `resolveType/decl/`      |
| `FuncSignatureResolver` | Function signature resolution                              | `resolveType/decl/`      |
| `ImplResolver`          | Impl target, generic arity, method resolution (complex)    | `resolveType/decl/`      |
| `FromResolver`          | From entry resolution (inline and path)                    | `resolveType/decl/`      |
| `VarResolver`           | Variable type resolution                                   | `resolveType/decl/`      |
| `InjectionTransformer`  | Injection form `!` type transformation                     | `resolveType/injection/` |
| `CallableExtractor`     | Function reference extraction from callable expressions    | `resolveType/callable/`  |
| `ResolverHelpers`       | Shared utilities (type classification, name extraction)    | `resolveType/helpers/`   |

### Generic Parameter and Substitution Management

Generic parameters and substitution maps use internal stacks managed by `GenericParamHandler`:

```cpp
// In GenericParamHandler
void pushParams(const ArenaSpan<GenericParamPtr>* params);
void popParams();
bool isParam(InternedString name) const;

void pushSubstMap(const std::unordered_map<InternedString, TypeAST*>* map);
void popSubstMap();
TypeAST* lookupSubst(InternedString name) const;
```

Constraint satisfaction uses `ConstraintChecker`:

```cpp
// In ConstraintChecker
bool satisfies(TypeAST* type, const std::vector<InternedString>& requiredTraits) const;
void setStructTraits(const std::unordered_map<InternedString, std::vector<InternedString>>* map);
```

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

Phase 3 walks top-level declarations, then recursively checks bodies via `checkStmt` and `checkExpr`. Expression checkers return `TypeAST*` and cache it on `node->resolvedType` in `ExprDispatcher.cpp`.

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
├── BindPattern      → introduce symbol in scope
├── WildcardPattern  → accept
├── LiteralPattern   → compare to subject type
├── TypePattern      → `is` check against subject
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
NameMangler::mangleType / mangleFunc                                       [helpers/NameMangler.hpp]
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
```

`ctx_.dispatcher` is wired so checkers can call `ctx.dispatcher->resolveType` for late or local type resolution inside Phase 3.

## Key Changes from Previous Structure

| Previous                           | New                                        |
| ---------------------------------- | ------------------------------------------ |
| `TypeResolver` monolithic class    | `TypeDispatcher` with delegated components |
| `TypeResolver.hpp/cpp` single file | 30 files across 9 subdirectories           |
| All resolve logic in one file      | Per-type resolvers in dedicated files      |
| `TypeChecker` in `resolveType/`    | `TypeChecker` moved to `checkType/`        |
| Generic params as member variables | `GenericParamHandler` component            |
| Clone methods inline               | `TypeCloner` component                     |
| Constraint checking inline         | `ConstraintChecker` component              |
| Injection transform inline         | `InjectionTransformer` component           |
| Callable extraction inline         | `CallableExtractor` component              |
```
