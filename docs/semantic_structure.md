
## Semantic phase roadmap

The semantic pass walks the AST produced by the parser and enforces all rules the grammar leaves open. Based on your project structure and AST design, here's the recommended file layout and the functions each file should own.

------

### File-by-file breakdown

**`src/semantic/SemanticSymbol.hpp`** — the symbol type itself

```cpp
enum class SymbolKind { Var, Func, Struct, Enum, Trait, TypeAlias, Param, Field, Method, EnumVariant };

struct Symbol {
    std::string    name;
    SymbolKind     kind;
    DeclKeyword    declKw;       // Let / Imt / Val (for Var/Func)
    Visibility     visibility;
    TypeAST*       type;         // resolved type (non-owning)
    BaseAST*       decl;         // back-pointer to the AST node
    bool           isAsync = false;
    SourceLocation loc;
};
```

---

**`src/semantic/SymbolTable.hpp` + `SymbolTable.cpp`**

The scope stack. Every block, function body, and impl block pushes a new scope.

```cpp
class SymbolTable {
public:
    void pushScope();
    void popScope();
    bool declare(const Symbol& sym);             // false = already declared in this scope
    Symbol* lookup(const std::string& name);     // walks scope stack outward
    Symbol* lookupLocal(const std::string& name);// current scope only

    int currentDepth() const;
private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes_;
};
```

---

**`src/semantic/SemanticCollector.hpp` + `SemanticCollector.cpp`**

**Phase 1** — first pass over the AST, no type checking yet. Collects all top-level names into the file-scope symbol table so forward references work (a function can call something declared below it in the same file).

```
collectProgram(ProgramAST&)
  collectTopLevelDecl(DeclAST&)
    collectVarDecl      → adds Symbol(Var)
    collectFuncDecl     → adds Symbol(Func), registers param names
    collectStructDecl   → adds Symbol(Struct), collects field names
    collectEnumDecl     → adds Symbol(Enum), collects EnumVariant symbols
    collectTraitDecl    → adds Symbol(Trait), stores method signatures
    collectImplDecl     → merges methods onto the struct's symbol, checks no duplicates
    collectTypeAlias    → adds Symbol(TypeAlias)
    collectExternDecl   → adds Symbol(Func, extern=true)
```

Run this over all files in the package before any checking begins, so cross-file `use` references can resolve.

---

**`src/semantic/TypeResolver.hpp` + `TypeResolver.cpp`**

**Phase 2a** — takes a raw `TypeAST*` produced by the parser and validates that every name inside it resolves to something in the symbol table. Returns the resolved `TypeAST*` (same node, now verified), or emits a diagnostic and returns a sentinel error type.

```
resolveType(TypeAST*) → TypeAST*
  resolvePrimitiveType  — always valid
  resolveNamedType      — looks up IDENTIFIER in symbol table, checks generic arg count
  resolveNullableType   — recurse inner; reject if enclosing decl is val
  resolveUnionType      — recurse each member, flatten nested unions
  resolveFixedArrayType — recurse element, check size > 0
  resolveSliceType      — recurse element
  resolveDynamicArrayType — recurse element
  resolveRefType        — recurse inner
  resolvePtrType        — reject unless insideExtern_ flag is set
  resolveFuncType       — recurse each param type and return type
```

---

**`src/semantic/TypeChecker.hpp` + `TypeChecker.cpp`**

**Phase 2b** — compatibility checks between two already-resolved types. This is a pure utility — no AST walking, just type comparisons. Both the expression checker and the statement checker call into this.

```
isAssignable(TypeAST* from, TypeAST* to) → bool
isCallable(TypeAST*) → bool
isBooleanCompatible(TypeAST*) → bool
isNullable(TypeAST*) → bool
hasNilInTree(TypeAST*) → bool        // used for val validation
unify(TypeAST* a, TypeAST* b) → TypeAST*   // for match arms, if/else branches
primitiveWidening(PrimitiveKind from, PrimitiveKind to) → bool
isFromConvertible(TypeAST* src, TypeAST* target) → bool  // checks pub impl from()
```

---

**`src/semantic/SemanticDecl.cpp`**

**Phase 3a** — walks declaration nodes, resolves their types via `TypeResolver`, checks declaration-level rules.

```
checkVarDecl
  - resolve annotation type
  - if val: call hasNilInTree(), error if true
  - if init present: check isAssignable(init type, annotation type)
  - if imt/val + no init: error (val always needs init; imt may in some cases)

checkFuncDecl
  - resolve param types and return type
  - push function scope, declare params
  - check body (delegates to SemanticStmt::checkBlock)
  - pop scope

checkStructDecl
  - resolve each field type
  - check no duplicate field names (parser should catch this, but double-check)

checkEnumDecl
  - validate explicit values are unique
  - assign auto-increment values to unassigned variants

checkTraitDecl
  - resolve method param and return types

checkImplDecl
  - for each method: resolve types, check body
  - if traitRef present: verify every trait method is implemented with matching signature
  - fromDecl: only valid on pub impl, check return type matches struct name

checkExternDecl
  - resolve param and return types
  - set insideExtern_ = true during resolution so @T is accepted
```

---

**`src/semantic/SemanticExpr.cpp`**

**Phase 3b** — the bulk of the work. Walks every expression, resolves its type, writes it onto `resolvedType`, enforces operator rules.

```
checkExpr(ExprAST*) → TypeAST*       // dispatches by node kind, returns resolved type

checkLiteralExpr        → PrimitiveTypeAST based on LiteralKind
checkIdentExpr          → lookup in symbol table; error if not found
checkArrayLiteralExpr   → infer element type; match declared type's kind (fixed/slice/dynamic)
checkStructLiteralExpr  → look up struct, verify all required fields provided, check each init type
checkBinaryExpr         → check operand types; validate operator is defined for them
checkUnaryExpr          → validate operator for operand type
checkCallExpr           → check callee is callable; match arg types to params; handle from() dispatch
checkIndexExpr          → check target is array, index is int; slice: check both ends
checkFieldAccessExpr    → resolve field on struct type; check it exists
checkBehaviorAccessExpr → resolve method on impl surface; mark isBehaviorMember = true
checkNullableChainExpr  → each step must be nullable type; fallback must match final resolved type
checkAssignExpr         → LHS must be let variable or let-held field; compound: desugar first
checkIsExpr             → set narrowed type flag on enclosing scope
checkPipelineExpr       → each step: resolve type, check upstream output matches step input
checkComposeExpr        → compile-time: each operand output must exactly match next input
checkAnonFuncExpr       → push scope, check params, check body, pop scope
checkAwaitExpr          → error if asyncDepth_ == 0 or parallelDepth_ > 0
checkMatchExpr          → check subject, check each arm pattern vs subject type, unify arm bodies
checkIfExpr             → check condition is bool; both branches must produce same type
checkRangeExpr          → check lo/hi are int (for loop/pattern) or same numeric type
checkTypeConvExpr       → safe: validate conversion path; unsafe: error if !insideExtern_
```

---

**`src/semantic/SemanticStmt.cpp`**

**Phase 3c** — walks statement nodes. Manages scope depth and context flags.

```
checkStmt(StmtAST*)      → dispatches

checkBlock               → pushScope, check each stmt, popScope; set scopeDepth on node
checkExprStmt            → checkExpr; warn if Result<T> discarded without ?? or catch
checkDeclStmt            → dispatch to checkVarDecl or checkFuncDecl
checkIfStmt              → check condition is bool; check thenBranch; check elseBranch if present
checkSwitchStmt          → check subject; check each case value type matches subject; check bodies
checkForStmt             → check iterable is collection or range; declare var in loop scope
checkWhileStmt           → check condition is bool; loopDepth_++, check body, loopDepth_--
checkDoWhileStmt         → loopDepth_++, check body, loopDepth_--; check condition is bool
checkReturnStmt          → check value type matches enclosing function return type
                           error if parallelDepth_ > 0
checkBreakStmt           → error if loopDepth_ == 0 or parallelDepth_ > 0
checkContinueStmt        → same as break
checkParallelForStmt     → parallelDepth_++; check no outer writes; check body; parallelDepth_--
checkParallelBlockStmt   → parallelDepth_++; check each sub-block; parallelDepth_--
```

---

**`src/semantic/SemanticAnalyzer.hpp` + `SemanticAnalyzer.cpp`**

The public entry point. Orchestrates the four phases across all files in the package.

```cpp
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(DiagnosticEngine& dc);

    // Run the full semantic pass over all files in a package.
    // Returns false if any errors were emitted.
    bool analyze(std::vector<ProgramAST*> files);

private:
    void resolveImports(std::vector<ProgramAST*>& files);  // check use decls, detect cycles
    void collectSymbols(std::vector<ProgramAST*>& files);  // Phase 1
    void resolveTypes(std::vector<ProgramAST*>& files);    // Phase 2
    void checkDecls(std::vector<ProgramAST*>& files);      // Phase 3
    void annotate(std::vector<ProgramAST*>& files);        // Phase 4

    SymbolTable    symbols_;
    TypeResolver   typeResolver_;
    TypeChecker    typeChecker_;
    DiagnosticEngine& dc_;

    bool insideExtern_ = false;
    int  asyncDepth_   = 0;
    int  loopDepth_    = 0;
    int  parallelDepth_= 0;
};
```

---

**`src/semantic/Annotator.cpp`**

**Phase 4** — a final visitor pass that writes `resolvedType`, `isConst`, `scopeDepth`, and `isBehaviorMember` onto every `BaseAST` node. By this point all errors are known, so this pass can be straightforward.

```
annotateNode(BaseAST*)
  → sets resolvedType from checkExpr results (stored on a side-map during phase 3)
  → sets isConst for literals and imt/val declarations
  → sets scopeDepth from SymbolTable::currentDepth()
  → sets isBehaviorMember on BehaviorAccessExprAST results
```

---

### Recommended build order

Start in this sequence — each step gives you something testable before moving on:

1. `SemanticSymbol.hpp` + `SymbolTable` — no AST walking yet, unit-testable standalone
2. `SemanticCollector` — first real AST walk; verify you can print all top-level symbol names
3. `TypeResolver` — test on individual `TypeAST*` nodes before hooking into declarations
4. `TypeChecker` — pure logic, easy to unit test
5. `SemanticDecl` (struct, enum, trait only — skip func bodies for now)
6. `SemanticExpr` (literals and identifiers first, then operators, then pipelines)
7. `SemanticStmt` (block + if + return first, loops next, parallel last)
8. Fill in `SemanticDecl` func/impl/extern bodies now that expr/stmt checkers exist
9. `Annotator`
10. Wire everything through `SemanticAnalyzer`

The parallel and async rules are the most isolated — you can stub out their errors and fill them in last without blocking everything else.