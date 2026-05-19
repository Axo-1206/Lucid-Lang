## Analysis: `checkTopLevelDecl`, `checkExpr`, and `checkStmt` Design

### Overview

The three functions form the core of Phase 3 (semantic checking) in the Luc compiler.  
- **`checkTopLevelDecl`** (SemanticDecl.cpp) dispatches to declaration‑specific checks (var, func, struct, enum, trait, impl, from).  
- **`checkExpr`** (SemanticExpr.cpp) evaluates expressions, sets `resolvedType`, and enforces operator rules.  
- **`checkStmt`** (SemanticStmt.cpp) handles statements, manages scopes, loops, and control flow.  

They are mutually recursive – expressions can contain statements (e.g. `if` expression), statements can contain expressions, and declarations may contain both.

### 1. `checkTopLevelDecl` – Declaration Dispatcher

**What it does:**  
Given a `DeclAST*`, it calls the appropriate declaration‑specific checker (`checkVarDecl`, `checkFuncDecl`, `checkStructDecl`, …). It passes down the symbol table, type resolver, diagnostic engine, and context flags (`loopDepth`, `parallelDepth`, `insideExtern`).

**Design assessment:**  
✅ *Correct concept* – A single dispatcher for all top‑level declarations is clean and extensible.  

❌ **Flaws:**  
- **Missing handling for local declarations** – `checkTopLevelDecl` is only called for file‑level declarations. Local declarations inside blocks (e.g. `let x = 5` inside a function) are handled by `checkDeclStmt` in `SemanticStmt.cpp`, which duplicates the logic. This violates DRY.  
- **Inconsistent parameter order** – Some declaration checks take `TypeResolver& resolver` before `StringPool& pool`, others after.  
- **`checkEnumDecl` and `checkTraitDecl` receive `TypeResolver` by value** – This copies the resolver (including its internal stacks) – clearly a bug.  
- **No generic‑parameter handling** – For generic functions/structs, the resolver’s generic parameter stack is not pushed/pop around the declaration check.  
- **Depends on outdated forward declarations** – The forward declarations of `checkExpr` and `checkStmt` at the top of the file do not match the actual signatures in `SemanticExpr.cpp` / `SemanticStmt.cpp` (e.g., missing `StringPool` parameter). This will cause linker errors.

**Recommendation for a fresh start:**  
- Merge declaration checking into a single pass that works for both top‑level and local declarations.  
- Use a consistent `SemanticContext` struct (containing `SymbolTable&`, `TypeResolver&`, `DiagnosticEngine&`, `StringPool&`, `loopDepth`, `parallelDepth`, `insideExtern`) to reduce parameter clutter.  
- Ensure generic‑parameter stacks are correctly pushed/popped inside each declaration visitor.

### 2. `checkExpr` – Expression Type Checker

**What it does:**  
Dispatches to expression‑specific handlers (literal, identifier, binary op, call, etc.), computes the expression’s type, and stores it in `node->resolvedType`. Returns the type (or `nullptr` on error).

**Design assessment:**  
✅ *Correct separation* – Each expression kind has its own static handler, which keeps the main dispatch simple.  
✅ *Uses `ASTKind` switch for fast dispatch* – Avoids dynamic casts.  

❌ **Major flaws:**  
- **Signature mismatch** – The forward declaration in `SemanticDecl.cpp` expects `TypeChecker checker`, but the actual definition in `SemanticExpr.cpp` expects `StringPool& pool`. This is a fundamental inconsistency that would prevent linking.  
- **Missing `StringPool` in many callers** – For example, `checkBinaryExpr` is called with `pool` twice (once as `pool`, once as `resolver`?) – look at the call in `checkExpr` dispatcher:  
  `return checkBinaryExpr(*node->as<BinaryExprAST>(), symbols, pool, resolver, pool, dc, ...)` – clearly a copy‑paste error.  
- **Relies on deprecated `SemanticHelpers`** – Many handlers call `SemanticHelpers::getPrimitiveType`, `SemanticHelpers::cloneType`, `SemanticHelpers::isInsideAsyncFunction`, etc., which are not implemented in the provided files (the header is marked deprecated and likely removed).  
- **No proper handling of `nullptr` return types** – For `nil` literals, `checkLiteralExpr` is completely commented out, so `nil` never gets a type.  
- **`checkCallExpr` is monolithic** – It tries to handle built‑in methods, extern functions, regular functions, and partial application in a single 200+ line function – very hard to maintain.  
- **Type checking logic duplicated** – Many expression handlers manually perform type compatibility checks (e.g., `isAssignable`) instead of delegating to a central `TypeChecker` instance that is passed around. The `TypeChecker` object is created locally in some places, leading to inconsistent state.

**Recommendation for a fresh start:**  
- Define a single, immutable `TypeChecker` instance (or make its methods static) that only depends on `SymbolTable`, `StringPool`, and `ASTArena`. Pass it explicitly, not via global state.  
- Remove all dependencies on `SemanticHelpers`. Move primitive type singletons into `TypeChecker` or a dedicated `PrimitiveTypes` singleton.  
- Ensure all expression handlers have access to `StringPool` (for name display) and `DiagnosticEngine` (for errors).  
- Break `checkCallExpr` into smaller helpers: `checkBuiltinMethodCall`, `checkExternCall`, `checkRegularCall`, `checkPartialApplication`.  
- Use a uniform signature: `TypeAST* checkExpr(ExprAST*, SemanticContext&)` where `SemanticContext` bundles all required references.

### 3. `checkStmt` – Statement Checker

**What it does:**  
Dispatches to statement handlers, manages block scopes (`pushScope`/`popScope`), tracks loop depth for `break`/`continue`, and validates return statements against the enclosing function’s expected return type.

**Design assessment:**  
✅ *Correct use of depth counters* – `loopDepth` and `parallelDepth` are correctly incremented/decremented.  
✅ *Proper scope management* – Blocks push/pop symbol scopes, and type narrowing in `if` statements is implemented (though only for `IsExpr`).  

❌ **Flaws:**  
- **Relies on missing `ParallelForStmtAST` and `ParallelBlockStmtAST`** – These statement kinds are not defined in the provided AST headers. The code would not compile.  
- **Uses `stmtPrimType` static function** – Duplicates the primitive type singleton pattern already present in `SemanticDecl.cpp`. Should be shared.  
- **`checkDeclStmt` re‑declares symbols** – It calls `checkVarDecl`/`checkFuncDecl` and then *again* declares the symbol in the current scope. This leads to double declaration and potential errors. The declaration should happen exactly once – either inside the `checkXxxDecl` or outside, but not both.  
- **No handling of `MultiVarDeclAST` and `MultiAssignStmtAST`** – These statement kinds are defined in `StmtAST.hpp` but never checked.  
- **Missing `checkStmt` for `ReturnStmt` with multiple values** – The implementation only checks the first returned value, ignoring the possibility of multi‑return.  
- **`checkParallelForStmt` and `checkParallelBlockStmt` are defined but the AST nodes are missing** – This indicates a mismatch between the semantic pass and the AST definition.

**Recommendation for a fresh start:**  
- Ensure all statement kinds defined in `StmtAST.hpp` have corresponding handlers.  
- Remove duplication: let `checkVarDecl` and `checkFuncDecl` *also* declare the symbol (or have a separate `declareSymbol` that is called from both places).  
- Use a `SemanticContext` that includes `expectedReturnType` (instead of passing it as a separate parameter) – but be careful because `expectedReturn` changes when entering functions.  
- Add proper handling of multi‑return values in `ReturnStmt` and multi‑assignment.

### Overall Architectural Issues

1. **Circular dependencies** – The forward declarations at the top of each `.cpp` file are a code smell. A proper header with a `SemanticContext` struct and function declarations would be cleaner and safer.  
2. **Deprecated `SemanticHelpers.hpp`** – The code still references it, but the file is marked deprecated and its contents are not provided. This makes the current code uncompilable.  
3. **No clear ownership of `TypeChecker`** – Some functions create a local `TypeChecker`, others receive it as a parameter, others ignore it. This leads to inconsistent type checking logic.  
4. **Missing generic parameter handling** – Generic functions and structs are not fully supported in Phase 3 because the generic parameter stacks are never pushed/popped during body checking.  
5. **No error recovery** – When an expression fails to type, `errorFallback` returns `Any`, but in many places the return value is ignored and the checker continues, potentially cascading errors.

### Conclusion

The **core idea** of splitting semantic checks into three mutually recursive functions is sound. However, the **current implementation** is severely flawed:  
- Inconsistent function signatures across files.  
- Heavy reliance on deprecated and missing helper code.  
- Duplicated logic and missing handlers for many AST nodes.  
- No proper generic type instantiation.  

For a **fresh start**, I recommend:  
1. Define a single `SemanticContext` struct that holds all shared state (`SymbolTable`, `TypeResolver`, `TypeChecker`, `DiagnosticEngine`, `StringPool`, `loopDepth`, `parallelDepth`, `insideExtern`, `expectedReturnType`).  
2. Declare the three checking functions in a common header with consistent signatures:  
   ```cpp
   void checkTopLevelDecl(DeclAST*, SemanticContext&);
   TypeAST* checkExpr(ExprAST*, SemanticContext&);
   void checkStmt(StmtAST*, SemanticContext&);
   ```  
3. Remove all dependencies on `SemanticHelpers` – move primitive type singletons into `TypeChecker` or a dedicated `PrimitiveTypes` class.  
4. Ensure every AST node kind has a dedicated checking function.  
5. Implement generic parameter stack management correctly inside `TypeResolver` and push/pop it before checking bodies.  
6. Write the checking functions in a purely recursive style without static local variables or global state.

This will produce a maintainable, correct semantic analysis phase that can be extended without the current tangled mess of forward declarations and mismatched signatures.