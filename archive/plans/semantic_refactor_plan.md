Looking at the codebase, I can see several significant discrepancies between the current files and what's actually being used. Let me read the key files more carefully before making a plan.Now I have a clear picture. The semantic files in the context window use a **completely different, refactored type system** than what the actual `TypeAST.hpp` on disk contains. The context files assume:

- `FuncTypeAST` has fields like `paramGroups` (of `ParamGroup`/`ParamInfo`), `returnType`, `isNullable`, `qualifiers` directly on the struct, `isAsync()` method
- `FuncDeclAST` has a `.type` field (a `FuncTypeAST`)
- `MethodDeclAST` has a `.type` field
- `ParamInfo` structs with `.name`, `.type`, `.isVariadic`, `.loc`

But the **actual** `TypeAST.hpp` has:
- `FuncTypeAST` with `FuncSignature sig` (which has `paramGroups` of `ASTPtr<ParamAST>`, `returnTypes` (plural), `qualifiers`)
- `FuncDeclAST` (in `DeclAST.hpp`) has `FuncSignature sig` and `StmtPtr body`
- Params are `ParamAST` nodes, not `ParamInfo`

Here's the full refactoring plan:

---

## Refactoring Plan: Semantic Files → Match Actual AST

### Root Cause

All semantic `.cpp` files and `SemanticHelpers.hpp`/`TypeResolver.hpp`/`TypeChecker.hpp` were written against a **planned refactored AST** (unified `FuncTypeAST` with `ParamInfo`, single `returnType`, `isNullable` flag, `node.type` on decls) that was never merged into the actual AST files. The actual AST uses `FuncSignature` with `paramGroups` of `ASTPtr<ParamAST>`, `returnTypes` (plural vector), and declarations embedding `FuncSignature sig` directly.

---

### Phase 1 — Audit & Mapping Table

Create a precise mapping of every wrong assumption:

| Semantic code assumes                                                             | Actual AST has                                                                       |
| --------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| `FuncTypeAST::paramGroups` (of `ParamInfo`)                                       | `FuncTypeAST::sig.paramGroups` (of `ASTPtr<ParamAST>`)                               |
| `FuncTypeAST::returnType` (single `TypePtr`)                                      | `FuncTypeAST::sig.returnTypes` (vector of `TypePtr`)                                 |
| `FuncTypeAST::isNullable`                                                         | `FuncTypeAST::sig.isNullable()` via `QualifierBits::Nullable`                        |
| `FuncTypeAST::qualifiers`                                                         | `FuncTypeAST::sig.qualifiers`                                                        |
| `FuncTypeAST::isAsync()`                                                          | `FuncTypeAST::sig.isAsync()`                                                         |
| `FuncTypeAST::rawQualifiers`                                                      | `FuncTypeAST::sig.rawQualifiers`                                                     |
| `FuncDeclAST::type` (a `FuncTypeAST`)                                             | `FuncDeclAST::sig` (a `FuncSignature`)                                               |
| `FuncDeclAST::bodyKind`, `FuncDeclAST::exprBody`                                  | Does not exist — body is always `StmtPtr body` (BlockStmtAST)                        |
| `MethodDeclAST::type`                                                             | `MethodDeclAST::sig` (a `FuncSignature`)                                             |
| `TraitMethodAST::type`                                                            | `TraitMethodAST::sig` (a `FuncSignature`)                                            |
| `ParamInfo` struct (name, type, isVariadic, loc)                                  | `ParamAST` node (name, type, isVariadic) via `ASTPtr<ParamAST>`                      |
| `ParamGroup` = `vector<ParamInfo>`                                                | `vector<ASTPtr<ParamAST>>`                                                           |
| `FromEntryAST::paramGroups` (of `ParamInfo`)                                      | `FromEntryAST::sig.paramGroups` (of `ASTPtr<ParamAST>`)                              |
| `FromEntryAST::returnTypeName` (string)                                           | `FromEntryAST::returnType` (TypePtr)                                                 |
| `AttributeArgAST::argKind`                                                        | `AttributeArgAST::kind` (the field is named `kind`)                                  |
| `AttributeArgAST::value` (string)                                                 | `AttributeArgAST::value` (InternedString — correct but needs pool lookup)            |
| `IntrinsicRegistry::lookup(name)` as static                                       | `IntrinsicRegistry::instance().lookup(name)`                                         |
| `FuncBodyKind` enum                                                               | Does not exist in actual AST                                                         |
| `node.exprBody` on FuncDeclAST                                                    | Does not exist                                                                       |
| `node.varName` on ForStmtAST                                                      | `node.iterVar->name` (a `ParamAST`)                                                  |
| `node.varType` on ForStmtAST                                                      | `node.iterVar->type`                                                                 |
| `node.value` on ReturnStmtAST (single ExprPtr)                                    | `node.values` (vector of ExprPtr)                                                    |
| `FieldInitAST` accessed as `.name`, `.value` directly                             | Correct — but note `node.inits` is `vector<FieldInitPtr>`                            |
| `StructLiteralExprAST::inits` as struct with `.name`, `.value`                    | `vector<ASTPtr<FieldInitAST>>` — each is a pointer, need `init->name`, `init->value` |
| `symbols.lookup(InternedString)`                                                  | `symbols.lookup(std::string)` — need `pool_.lookup(id)` conversion                   |
| `sym.name` compared as string directly                                            | `sym.name` is a `std::string` — OK                                                   |
| `node.name` on decls compared directly                                            | `node.name` is `InternedString` — need pool lookup for string ops                    |
| `QualifierRegistry::instance().getBit(qualName)` where qualName is InternedString | Need `std::string(pool.lookup(qualName))` conversion                                 |

---

### Phase 2 — Files to Modify (in dependency order)

#### 2.1 `TypeChecker.hpp` / `TypeChecker.cpp`

**Problems:**
- References `FuncTypeAST::paramGroups`, `::returnType`, `::isNullable`, `::qualifiers` directly instead of via `.sig`
- `isEqual()` for `FuncTypeAST` uses `fa->paramGroups`, `fa->returnType`, `fa->isNullable`
- `isAssignable()` for `FuncTypeAST` calls `isEqual()`
- `isNullable()` checks `type->as<FuncTypeAST>()->isNullable` — should be `->sig.isNullable()`
- `isCallable()` is fine (just checks `isa<FuncTypeAST>()`)

**Changes needed in `TypeChecker.cpp`:**
```cpp
// isEqual — FuncTypeAST branch:
// Change: fa->qualifiers  →  fa->sig.qualifiers
// Change: fa->isNullable  →  fa->sig.isNullable()
// Change: fa->paramGroups →  fa->sig.paramGroups  (but ParamAST not ParamInfo!)
// Change: fa->returnType  →  fa->sig.returnTypes (handle single vs multiple)
// groupA[i].type.get()   →  groupA[i]->type.get()  (ASTPtr<ParamAST>, not ParamInfo)

// isNullable:
// Change: type->as<FuncTypeAST>()->isNullable  →  type->as<FuncTypeAST>()->sig.isNullable()
```

**Changes needed in `TypeChecker.hpp`:**
- No structural changes, just ensure `isNullable` signature is correct

#### 2.2 `TypeResolver.hpp` / `TypeResolver.cpp`

**Problems:**
- `visit(FuncTypeAST&)` accesses `node.paramGroups`, `node.rawQualifiers`, `node.qualifiers`, `node.returnType`, `node.isNullable` — all should be via `.sig`
- `visit(FuncDeclAST&)` accesses `node.type` (doesn't exist — should be `node.sig`, and update symbol type to point to a synthesized `FuncTypeAST` or use the sig directly)
- `visit(MethodDeclAST&)` accesses `node.type` — should be `node.sig`
- `visit(TraitDeclAST&)` calls `resolveType(&method->type)` — should be `resolveType` on the sig's pieces or a synthesized node
- `visit(TraitMethodAST&)` accesses `node.type` — should be `node.sig`
- `visit(ImplDeclAST&)` accesses `method->type` — should be `method->sig`
- `visit(FromDeclAST&)` accesses `entry->paramGroups[g][i].type.get()` as ParamInfo — should use `entry->sig.paramGroups[g][i]->type.get()` (ParamAST pointer)
- `resolveStructFields` calls `resolver.resolveType` on field types — mostly fine
- The `SubstitutionMap` keyed by `std::string` — `GenericParamAST::name` is `InternedString`, so comparisons need pool lookups

**Key insight for FuncDecl/MethodDecl:** Since declarations use `FuncSignature sig` instead of `FuncTypeAST type`, the TypeResolver needs to either:
- Resolve directly against the `FuncSignature` fields (call `resolveType` on each param and return type)
- Or synthesize a `FuncTypeAST` wrapper to return as `resolved_`

The cleanest approach: add a helper `resolveFuncSignature(FuncSignature& sig)` that iterates params and return types, and have `visit(FuncDeclAST&)`, `visit(MethodDeclAST&)` etc. call it.

#### 2.3 `SemanticHelpers.hpp`

**Problems:**
- `cloneType(FuncTypeAST*)` accesses `f->isNullable`, `f->qualifiers`, `f->returnType`, `f->paramGroups` — all via `.sig`
- `param.name`, `param.type`, `param.isVariadic`, `param.loc` treat params as `ParamInfo` — should access `ParamAST` fields via pointer: `param->name`, `param->type`, `param->loc`, `param->isVariadic`
- `declareFunctionParameters(FuncTypeAST& type, ...)` iterates `type.paramGroups` as ParamInfo — fix to iterate `type.sig.paramGroups` with `ASTPtr<ParamAST>`
- `resolveFunctionType(FuncTypeAST& type, ...)` same issue
- `getFunctionReturnType` accesses `type.returnType` — should be `type.sig.returnTypes[0]` (or handle multiple)
- `FunctionContext::isInsideAsync()` calls `current_->type->as<FuncTypeAST>()->isAsync()` — but `Symbol::type` points to a `TypeAST*`, not necessarily a `FuncTypeAST` from a declaration. Need to handle the case where it's a raw `FuncTypeAST` (sig approach)
- `buildLocalFuncSignature` references `node.type.paramGroups` etc. — entire function needs rewriting using `node.sig`
- Remove `FuncBodyKind` enum — doesn't exist in actual AST
- `checkFunctionLikeDeclaration` takes `FuncBodyKind bodyKind, StmtPtr& body, ExprPtr& exprBody` — simplify to just `StmtPtr& body`

#### 2.4 `SemanticCollector.cpp`

**Problems:**
- `visit(FuncDeclAST&)`: `sym.type = &node.type` — `node.type` doesn't exist; should store reference to the signature somehow. Best approach: synthesize a `FuncTypeAST` or store a pointer to the sig via a helper. Actually, since `Symbol::type` is `TypeAST*`, we need a `FuncTypeAST*`. The cleanest fix: the semantic pass should **not** store `&node.type` directly, instead either: (a) lazily synthesize a `FuncTypeAST` on the `FuncDeclAST` itself, or (b) use `nullptr` and let Phase 2 fill it in
- `extractExternAttr` accesses `attr->args[0].argKind` — should be `attr->args[0]->kind` (it's `ASTPtr<AttributeArgAST>`) and `.value` is `InternedString` needing pool lookup
- `visit(TraitDeclAST&)`: `sym.type = &method->type` — `method->type` doesn't exist; fix same as FuncDecl
- `visit(ImplDeclAST&)`: `sym.type = &method->type` — same
- `visit(FromDeclAST&)`: `entry->paramGroups` — should be `entry->sig.paramGroups`
- InternedString name comparisons like `attr->name == "extern"` — `attr->name` is `InternedString`, needs pool lookup or intern comparison

**Strategy for `Symbol::type` for functions:** The cleanest fix without restructuring `Symbol` is to make `FuncDeclAST`, `MethodDeclAST`, and `TraitMethodAST` each hold a synthesized `FuncTypeAST` that wraps the sig. This can be a `mutable ASTPtr<FuncTypeAST> cachedType` (similar to `StructDeclAST::selfType`). Phase 1 creates it lazily.

#### 2.5 `SemanticDecl.cpp`

**Problems (many):**
- `resolveFunctionType(FuncTypeAST& type, ...)` — needs to accept `FuncSignature&` or `FuncTypeAST&` with `.sig` access
- `declareFunctionParameters` — same
- `checkFunctionLikeDeclaration` — takes `FuncBodyKind`, `exprBody` — remove these, simplify to just `StmtPtr& body`
- `checkFuncDecl`: accesses `node.type` (FuncTypeAST), `node.bodyKind`, `node.exprBody` — fix to `node.sig`, remove body kind handling
- `checkStructDecl`, `checkEnumDecl`, `checkTraitDecl`, `checkImplDecl`, `checkFromDecl` — various `.type` accesses on methods, param ParamInfo access
- `checkAttributes`: `attr->args[0].argKind` → `attr->args[0]->kind`; `.value` needs pool for string comparison; `attr->name == "extern"` needs pool comparison
- `checkImplDecl`: `method->type.isAsync()` → `method->sig.isAsync()`; `sym.type = &method->type` needs the cached FuncTypeAST approach
- `checkFromDecl`: `entry->paramGroups` → `entry->sig.paramGroups`; `entry->returnTypeName` → use `entry->returnType` directly; params accessed as ParamInfo → fix to ParamAST
- `isConstExpr` is fine structurally
- Return type handling: everywhere `returnType` is singular needs to handle `sig.returnTypes` vector (treat `returnTypes[0]` as the canonical single return, or introduce multiple-return checking)

#### 2.6 `SemanticStmt.cpp`

**Problems:**
- `checkForStmt`: `node.varName` → `pool.lookup(node.iterVar->name)` (or just `node.iterVar->name` as InternedString); `node.varType` → `node.iterVar->type.get()`
- `checkReturnStmt`: `node.value` (single ExprPtr) → `node.values[0]` (vector); bare return check: `node.values.empty()`
- `checkParallelForStmt`, `checkParallelBlockStmt`: reference `ParallelForStmtAST`, `ParallelBlockStmtAST` — these don't exist in `StmtAST.hpp`! These nodes need to be removed from the semantic checker (or are planned future nodes)
- `buildLocalFuncSignature` in SemanticStmt: accesses `node.type.paramGroups` etc. — fix to `node.sig`
- `checkDeclStmt`: `node.asVar()`, `node.asFunc()` — these convenience methods don't exist on `DeclStmtAST`; use `node.decl->as<VarDeclAST>()` etc. Symbol type assignment: `sym.type = &fd->type` → needs cached FuncTypeAST

#### 2.7 `SemanticExpr.cpp`

**Problems:**
- `checkCallExpr`: accesses `funcDecl->type.paramGroups`, `funcDecl->type.returnType` etc. — fix to `funcDecl->sig`; `calleeSym->type->as<FuncTypeAST>()->isAsync()` — sym type may not be FuncTypeAST directly
- `checkAssignExpr`: large block accesses `funcDecl->type.paramGroups`, `funcDecl->type.returnType`, `funcDecl->type.isAsync()` — fix to `funcDecl->sig`
- `checkAnonFuncExpr`: accesses `node.type.rawQualifiers`, `node.type.paramGroups`, `node.type.returnType` — fix to `node.sig`
- `checkAwaitExpr`: `callExpr->isAsyncCall` — this field doesn't exist on `CallExprAST`! Need to check via callee symbol's sig
- `checkComposeExpr`: extensively uses `FuncTypeAST::paramGroups` as `ParamInfo` — major rewrite needed; also `FuncTypeAST(isNullable)` constructor doesn't exist (default constructor only)
- `getBuiltinTypeKey(TypeAST*)` — called but not defined in these files (must be in a header somewhere)
- `checkStructLiteralExpr`: `init.name`, `init.value` — should be `init->name`, `init->value` (FieldInitPtr dereference)
- `checkBehaviorAccessExpr`: `node.typeName` and `node.method` are `InternedString` — string ops need pool; comparison `lhsSym->name` is `std::string` so `symbols.lookup(std::string(pool.lookup(node.typeName)))` pattern needed

#### 2.8 `Annotator.cpp`

**Problems:**
- References `FuncDeclAST::type.paramGroups`, `node.type.returnType` — fix to `node.sig`
- References `ParallelForStmtAST`, `ParallelBlockStmtAST` — remove
- `node.varName` on ForStmtAST → `node.iterVar->name`
- `ReturnStmtAST::value` → `node.values`
- `DeclStmtAST::asVar()`, `asFunc()` — fix to `node.decl->isa<>()` / `->as<>()`
- `StructLiteralExprAST::inits` accessed as `init.value` → `init->value`

---

### Phase 3 — New Helper Needed: `FuncTypeAST` Synthesis

Since `Symbol::type` must be `TypeAST*`, and function declarations use `FuncSignature sig` (not `FuncTypeAST`), add a synthesized type cache to the three declaration types that need it:

**Add to `FuncDeclAST`** (in `DeclAST.hpp`):
```cpp
mutable ASTPtr<FuncTypeAST> cachedType;  // synthesized FuncTypeAST for Symbol::type
```

**Add to `MethodDeclAST`** similarly.

**Add to `TraitMethodAST`** similarly.

Then in `SemanticCollector`, when registering these symbols, call a helper that builds the `cachedType` from the `FuncSignature` if it's null, and store `node.cachedType.get()` as `sym.type`.

---

### Phase 4 — `InternedString` / `std::string` Bridge

Throughout, `node.name` on declarations is `InternedString`. The symbol table uses `std::string` keys. Need a consistent pattern:

```cpp
// For lookups:
symbols.lookup(std::string(pool_.lookup(node.name)))

// For symbol registration:
sym.name = std::string(pool_.lookup(node.name))

// For attribute name comparisons:
std::string(pool_.lookup(attr->name)) == "extern"
```

This pattern needs to be applied consistently wherever `InternedString` fields are used as `std::string`.

---

### Phase 5 — Multiple Return Types

`FuncSignature::returnTypes` is a `vector<TypePtr>`. The semantic pass (especially `checkReturnStmt`, `checkFuncDecl` return type checking, `TypeChecker::isEqual` for FuncTypeAST) was written assuming a single `returnType`. The plan:

- For **Phase 3 semantic checking**: treat `returnTypes[0]` as the primary return type for single-return functions; for multi-return, the `ReturnStmtAST::values` vector must match the count and types of `returnTypes`
- For **TypeChecker::isEqual** on `FuncTypeAST`: compare `sig.returnTypes` vectors element-by-element
- Update `checkReturnStmt` to iterate `node.values` against `expectedReturn` (which may need to become a `vector<TypeAST*>` eventually, but for now pass as single for single-return compatibility)

---

### Phase 6 — Remove Non-Existent Nodes

These AST node types are referenced in semantic files but **do not exist** in `StmtAST.hpp`:
- `ParallelForStmtAST` 
- `ParallelBlockStmtAST`

Remove all references to them from `SemanticStmt.cpp`, `Annotator.cpp`, and `SemanticHelpers.hpp`. The parallel feature is simply not implemented in the AST yet.

Similarly, `CallExprAST::isAsyncCall` doesn't exist — remove that field reference.

---

### Execution Order

1. **`DeclAST.hpp`** — Add `mutable ASTPtr<FuncTypeAST> cachedType` to `FuncDeclAST`, `MethodDeclAST`, `TraitMethodAST`
2. **`TypeChecker.cpp/.hpp`** — Fix `isEqual`, `isNullable` to use `.sig`
3. **`TypeResolver.hpp/.cpp`** — Fix all `.type` → `.sig` accesses; add `resolveFuncSignature(FuncSignature& sig)` helper; fix ParamAST dereference
4. **`SemanticHelpers.hpp`** — Fix `cloneType`, `declareFunctionParameters`, `resolveFunctionType`, remove `FuncBodyKind`, remove parallel node refs, fix param access
5. **`SemanticCollector.cpp`** — Fix attribute arg access, InternedString→string, use `cachedType`
6. **`SemanticDecl.cpp`** — Fix all sig/type confusion, remove `bodyKind`/`exprBody`, fix attr args, fix param access, fix ForStmt, fix ReturnStmt
7. **`SemanticStmt.cpp`** — Fix ForStmt, ReturnStmt, remove parallel nodes, fix DeclStmt helpers
8. **`SemanticExpr.cpp`** — Fix FuncTypeAST field access, fix FieldInitPtr dereference, fix BehaviorAccess string ops, rewrite compose expr
9. **`Annotator.cpp`** — Fix all references to non-existent fields, remove parallel nodes

---

### Summary

The core problem is a **half-completed refactor** from the old `FuncTypeAST`-embedded-in-declarations design to a unified `FuncSignature`-on-declarations design, where the semantic layer was rewritten against the new design but the AST was never fully migrated. The fix requires:

1. Adding `cachedType` synthesized `FuncTypeAST` wrappers to the three declaration types so `Symbol::type` can hold a valid pointer
2. Systematically replacing every `.type.X` on declarations with `.sig.X`  
3. Fixing all `ParamInfo` accesses to `ParamAST*` dereferences
4. Fixing `InternedString` → `std::string` conversions for symbol table and string comparisons
5. Handling `returnTypes` (plural) vector instead of single `returnType`
6. Removing references to non-existent AST nodes (`ParallelForStmtAST`, etc.)