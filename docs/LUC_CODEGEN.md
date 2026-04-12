# Luc — Codegen Phase

> **Scope of this file:** Architecture, file responsibilities, three-pass strategy,
> type lowering, generics (monomorphization), memory model, closure/curry ABI,
> and the runtime contract for the code generation phase.
> The semantic phase that feeds into codegen is documented in `LUC_SEMANTIC.md`.

---

## Overview

The codegen phase translates the fully annotated, semantically-validated AST into
LLVM IR and then hands it to LLVM's ORC JIT engine for execution. It is the final
compiler phase before the program runs.

The phase reads three kinds of information stamped onto the AST by the semantic
pass:

| Field | Set by | Used by codegen for |
|---|---|---|
| `resolvedType` | Phase 3b (`checkExpr`) | Choosing the right LLVM type and instruction |
| `isBehaviorMember` | Phase 3b + Phase 4 (Annotator) | Blocking reassignment, mangled name lookup |
| `isConst` | Phase 4 (Annotator) | Emitting `llvm::Constant*` instead of alloca/store |
| `scopeDepth` | Phase 3c (`checkBlock`) | Assertion sanity checks during block lowering |

---

## File Structure

```
src/codegen/
├── CodeGen.hpp            # Driver — owns LLVMContext, Module, IRBuilder, JIT
├── CodeGen.cpp            # Orchestration, three-pass entry point, JIT execution
├── CodeGenDecl.cpp        # Pass 1 (type+sig forward-decl) + Pass 2 (bodies)
├── CodeGenExpr.cpp        # Expression lowering — lowerExpr()
├── CodeGenStmt.cpp        # Statement lowering — lowerStmt()
├── CodeGenType.cpp        # TypeAST* → llvm::Type* mapping
└── ValueEnv.hpp           # Scope stack: name → llvm::Value*, struct registry,
                           #   func registry, loop-exit/continue block stack
```

The split mirrors the semantic phase exactly: `CodeGenDecl`, `CodeGenExpr`, and
`CodeGenStmt` communicate via the same forward-declaration pattern used by
`SemanticDecl.cpp`, `SemanticExpr.cpp`, and `SemanticStmt.cpp`. No headers are
needed for these sub-files — the linker connects them.

---

## File Responsibilities

| File | Responsibility |
|:---|:---|
| **`CodeGen.hpp/cpp`** | Driver. Owns `LLVMContext`, `llvm::Module`, `llvm::IRBuilder<>`, and the ORC JIT engine. Runs the three lowering passes over all files in the package, then calls `runJIT()`. |
| **`ValueEnv.hpp`** | Scope stack mapping names to `llvm::Value*`. Also holds the struct-type registry (`name → llvm::StructType*`), the function registry (`name → llvm::Function*`), the generic instantiation registry (see Generics), and the loop-control stack (exit/continue `BasicBlock*` pairs). Mirrors `SymbolTable` exactly in interface. |
| **`CodeGenType.cpp`** | Pure type mapping. `lowerType(TypeAST*, ctx, env, subst)` converts every TypeAST variant to the corresponding `llvm::Type*`. Accepts an optional type-substitution map for generic instantiation — when a `NamedTypeAST` matches a generic param name the map is consulted instead of the struct registry. No IR is emitted here — only type objects are produced. |
| **`CodeGenDecl.cpp`** | Three-pass declaration lowering. Pass 0 collects all generic instantiations needed. Pass 1 forward-declares all concrete struct layouts and function signatures (including instantiated generics). Pass 2 fills in function bodies by delegating to `CodeGenStmt`. Also handles `ExternDeclAST` (declare-only, no body) and `FromDeclAST` (each entry becomes a function). |
| **`CodeGenExpr.cpp`** | Expression lowering. `lowerExpr(ExprAST*, ...)` dispatches on `ASTKind` and returns an `llvm::Value*`. Handles all operators, calls, field access, pipelines, match, type conversions, and generic call instantiation. |
| **`CodeGenStmt.cpp`** | Statement lowering. `lowerStmt(StmtAST*, ...)` handles blocks, if/switch/for/while/do, return, break, continue, and parallel stubs. Manages `loopStack_` on `ValueEnv` for break/continue targets. |

---

## The Three-Pass Strategy

A single-pass walk cannot emit a call to a function declared later in the same
file, or to a function in another file of the same package — and it cannot emit
a generic instantiation before knowing all the concrete type arguments needed
across the whole program. Codegen solves both problems with three passes, directly
mirroring how the semantic phase uses `SemanticCollector` (Phase 1) before
`checkDecls` (Phase 3).

### Pass 0 — Generic instantiation collection

Walks every `ProgramAST*` file without emitting any IR. Its only job is to
discover every unique `(baseName, [concreteTypeArgs])` combination that is actually
used in the program, so Pass 1 can emit concrete structs and function signatures
for all of them.

For each use site encountered:
- `NamedTypeAST` with non-empty `genericArgs` (e.g. `Buffer<int>`, `Vec2<float>`) → record the instantiation `("Buffer", [i32])`.
- `CallExprAST` with non-empty `genericArgs` (e.g. `process<int>(x)`) → record the function instantiation `("process", [i32])`.
- `StructLiteralExprAST` whose type is generic → record the struct instantiation.

Each unique `InstKey` is stored in `ValueEnv`'s instantiation registry. Non-generic
declarations (the majority of the program) are unaffected.

### Pass 1 — Forward declarations

Iterates all files without emitting any function bodies. For non-generic
declarations this is identical to the original two-pass strategy:

1. For every non-generic `StructDeclAST`: create `llvm::StructType::create(ctx, name)`, call `setBody()` with field types from `lowerType()`. Register in `ValueEnv`.
2. For every generic `StructDeclAST`: for each `InstKey` recorded in Pass 0 that matches this struct, emit one concrete `llvm::StructType` with field types substituted using `lowerType(field->type, ctx, env, subst)` where `subst` maps generic param names to concrete `llvm::Type*`. Register under the mangled name (e.g. `"Vec2<float>"`).
3. For every `EnumDeclAST`: register each variant as a named `llvm::ConstantInt`. The enum type itself is `i8` (≤ 255 variants) or `i16` (more).
4. For every non-generic `FuncDeclAST`, `MethodDeclAST`, `FromEntryAST`, and `ExternDeclAST`: build the `llvm::FunctionType` and create the `llvm::Function` with the correct mangled name. Register in `ValueEnv`.
5. For every generic `FuncDeclAST` or `MethodDeclAST`: for each matching `InstKey` from Pass 0, emit one concrete `llvm::Function` with param/return types substituted. Register under the mangled instantiation name (e.g. `"process<int>"`).

After Pass 1, the entire package's type and function surface — including all
generic instantiations — is visible to the IR builder regardless of source order.

### Pass 2 — Function bodies

Iterates all files again, filling in bodies. For each `llvm::Function*` created in
Pass 1 (both non-generic and every instantiation of generic functions):

1. Retrieve the `llvm::Function*` from Pass 1.
2. Create an `entry` `BasicBlock` and point `IRBuilder` at it.
3. `alloca` + `store` each parameter (enables `mem2reg` to promote them to SSA registers).
4. If the function is a generic instantiation, push the type-substitution map onto a context stack so `lowerType` and `lowerExpr` resolve generic param names to the correct concrete types throughout the body.
5. Call `lowerStmt(node->body.get(), ...)` to generate the block.
6. Pop the substitution map if pushed.
7. After `lowerStmt` returns, insert a defensive terminator if the last `BasicBlock` has none (`CreateRetVoid()` or `CreateRet(getNullValue(retType))`).

`ExternDeclAST` nodes are skipped entirely in Pass 2 — they have no body.

---

## Naming Convention

Functions are emitted with mangled names that match the convention already
established by `SemanticCollector` in Phase 1 of the semantic pass:

| Declaration | Mangled LLVM name |
|---|---|
| `let main () int` | `main` (no mangling — C ABI entry point) |
| `let add (a int) (b int) int` | `add` |
| `impl Vec2 { length () float }` | `Vec2.length` |
| `impl Vec2 { dot (other Vec2) float }` | `Vec2.dot` |
| `from Fahrenheit { celsius (c Celsius) Fahrenheit }` | `Fahrenheit.from.celsius` |
| `struct Vec2<T>` instantiated as `Vec2<float>` | `Vec2<float>` (struct type name) |
| `let process<T> (x T) string` instantiated with `int` | `process<int>` |
| `impl<T> Buffer<T> { push (v T) }` instantiated with `float` | `Buffer<float>.push<float>` |

The `from` mangling mirrors `SemanticCollector::visit(FromDeclAST&)` exactly:
`targetTypeName + ".from." + entry->name`.

`main` is a special case: it must match C's `int main()` for the OS loader and the
LLVM JIT's `lookup("main")` call. The semantic pass already enforces the correct
signature (`export imt main () int`), so codegen can safely emit it unmangled.

---

## ValueEnv

`ValueEnv` is the codegen equivalent of `SymbolTable`. It is a scope stack of
`unordered_map<string, llvm::Value*>` plus flat registries that live outside
the scope stack:

```cpp
// Key for a generic instantiation: base name + ordered list of concrete LLVM types.
// e.g. ("Vec2", [float, float])  or  ("process", [i32])
struct InstKey {
    std::string              baseName;
    std::vector<llvm::Type*> typeArgs;
    bool operator==(const InstKey&) const;  // element-wise comparison
};

// Type substitution map — maps generic param name to concrete llvm::Type*.
// e.g. { "T" → float,  "K" → i32 }
using TypeSubst = std::unordered_map<std::string, llvm::Type*>;

class ValueEnv {
public:
    // Scope stack — for named locals and parameters
    void         pushScope();
    void         popScope();
    void         define(const std::string& name, llvm::Value* val);
    llvm::Value* lookup(const std::string& name) const; // walks outward like SymbolTable

    // Struct type registry — populated in Pass 1, read throughout Pass 2
    void              defineType(const std::string& name, llvm::StructType* ty);
    llvm::StructType* lookupType(const std::string& name) const;

    // Function registry — populated in Pass 1, read throughout Pass 2
    void            defineFunc(const std::string& name, llvm::Function* fn);
    llvm::Function* lookupFunc(const std::string& name) const;

    // Generic instantiation registry — populated in Pass 0, consumed in Pass 1
    // Records every unique (baseName, typeArgs) combination needed by the program.
    void                         recordInst(const InstKey& key);
    const std::vector<InstKey>&  instsFor(const std::string& baseName) const;
    bool                         hasInst(const InstKey& key) const;

    // Type substitution stack — pushed/popped around generic function bodies in Pass 2
    void            pushSubst(const TypeSubst& subst);
    void            popSubst();
    llvm::Type*     resolveSubst(const std::string& paramName) const; // nullptr = not a param
    bool            inGenericBody() const;

    // Loop control — pushed/popped by for/while/do lowering in CodeGenStmt
    void              push_loop(llvm::BasicBlock* exitBB, llvm::BasicBlock* continueBB);
    void              pop_loop();
    llvm::BasicBlock* current_loop_exit()     const; // for break
    llvm::BasicBlock* current_loop_continue() const; // for continue

private:
    std::vector<std::unordered_map<std::string, llvm::Value*>>   scopes_;
    std::unordered_map<std::string, llvm::StructType*>           types_;
    std::unordered_map<std::string, llvm::Function*>             funcs_;
    std::map<std::string, std::vector<InstKey>>                  insts_;   // baseName → keys
    std::vector<TypeSubst>                                       substStack_;
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loopStack_;
};
```

---

## CodeGenType — TypeAST → llvm::Type*

```cpp
// subst is empty for non-generic contexts.
// When lowering inside a generic instantiation, subst maps param names to
// concrete llvm::Type* — e.g. { "T" → float } for a Vec2<float> instantiation.
llvm::Type* lowerType(TypeAST* node, llvm::LLVMContext& ctx,
                      ValueEnv& env, const TypeSubst& subst = {});
```

| TypeAST | LLVM Type | Notes |
|---|---|---|
| `PrimitiveTypeAST(Bool)` | `i1` | |
| `PrimitiveTypeAST(Byte / Int8)` | `i8` | |
| `PrimitiveTypeAST(Short / Int16)` | `i16` | |
| `PrimitiveTypeAST(Int / Int32)` | `i32` | |
| `PrimitiveTypeAST(Long / Int64)` | `i64` | |
| `PrimitiveTypeAST(Ubyte / Uint8)` | `i8` | same bits, sign handled by instruction choice |
| `PrimitiveTypeAST(Ushort / Uint16)` | `i16` | |
| `PrimitiveTypeAST(Uint / Uint32)` | `i32` | |
| `PrimitiveTypeAST(Ulong / Uint64)` | `i64` | |
| `PrimitiveTypeAST(Float)` | `float` | IEEE 754 32-bit |
| `PrimitiveTypeAST(Double)` | `double` | IEEE 754 64-bit |
| `PrimitiveTypeAST(Decimal)` | `{ i64, i64 }` | 128-bit, software-emulated |
| `PrimitiveTypeAST(String)` | `{ i8*, i64 }` | heap pointer + byte length |
| `PrimitiveTypeAST(Char)` | `i32` | Unicode codepoint |
| `PrimitiveTypeAST(Any)` | `{ i8*, i64 }` | data pointer + type tag (type registry index) |
| `NamedTypeAST` (non-generic) | `env.lookupType(name)*` | looked up from Pass 1 struct registry |
| `NamedTypeAST` (is a generic param, e.g. `T`) | `subst.at("T")` | resolved from the substitution map; error if not found |
| `NamedTypeAST` (generic instantiation, e.g. `Vec2<float>`) | `env.lookupType("Vec2<float>")*` | looked up under the mangled instantiation name |
| `NullableTypeAST(T)` | `{ T, i1 }` | value + `has_value` flag |
| `FixedArrayTypeAST([N]T)` | `[N x T]` | stack-allocated, size baked in |
| `SliceTypeAST([]T)` | `{ T*, i64, i64 }` | ptr + len + cap (fat pointer, no ownership) |
| `DynamicArrayTypeAST([*]T)` | `{ T*, i64, i64 }` | heap ptr + len + cap |
| `RefTypeAST(&T)` | `T*` | LLVM pointer to T |
| `PtrTypeAST(*T)` | `T*` | raw C pointer — same representation, different semantics |
| `FuncTypeAST` | `llvm::FunctionType*` | built from param + return types; nullable → `{ fn_ptr, i1 }` |
| `UnionTypeAST` | `{ i8, [ N x i8 ] }` | tag byte + payload sized to largest member |

**Nullable function type** (`((int) string)?`) lowers to `{ (i32) i8*, i1 }` — a
function pointer plus a `has_value` flag — not a `NullableTypeAST` wrapping a
`FuncTypeAST`.

**Union types** use a tagged-union layout: a one-byte discriminant followed by a
byte array large enough to hold the largest member. Alignment padding is inserted
by LLVM via `llvm::StructType::get()` when the types are provided in order.

---

## Memory Model

### Structs — value semantics, deep copy

Luc structs are value types. Every assignment copies the entire struct. Codegen
implements this as a `memcpy` into a fresh `alloca`. Fields that hold `&T`
(references) copy only the pointer — the copy does not own the pointed-to data,
exactly matching the language rule.

When a struct field is accessed (`.`), codegen emits a `CreateStructGEP` using the
field's index in the `StructDeclAST::fields` vector. The field index is looked up
by name from the `StructDeclAST` stored on the `Symbol`. This means the field
order in source is the field order in the LLVM struct — no reordering.

### Arrays

| Kind | Heap? | Codegen strategy |
|---|---|---|
| Fixed `[N]T` | No | `alloca [N x T]`, `getelementptr` for indexing |
| Slice `[]T` | No (view) | `alloca { T*, i64, i64 }`, GEP slice from source array |
| Dynamic `[*]T` | Yes | `{ T*, i64, i64 }` struct; mutations call into `luc_runtime` |

Dynamic array operations (`push`, `pop`, `insert`, `remove`, `clear`, `reserve`)
are lowered to calls into the Luc runtime (see Runtime Contract below). The array
struct is passed by pointer so the runtime can mutate `ptr`, `len`, and `cap`
in-place.

### Nullable types

`T?` lowers to `{ T, i1 }`. The `??` operator lowers to:

```
; entry:
  %has_val = extractvalue { T, i1 } %nullable, 1
  br %has_val, then_bb, else_bb

; then_bb:
  %val = extractvalue { T, i1 } %nullable, 0
  br merge_bb

; else_bb:
  %fallback = [lower fallback expr]
  br merge_bb

; merge_bb:
  %result = phi T [ %val, then_bb ], [ %fallback, else_bb ]
```

A nil literal lowers to `insertvalue { T, i1 } undef, i1 0, 1` — the value field
is undefined (never read), the flag is `false`.

### Enums

Each variant is a named `llvm::ConstantInt`. The enum type itself is `i8` (≤ 255
variants) or `i16` (more). `Direction.North` lowers to the constant integer value
assigned by `checkEnumDecl` in the semantic pass. Enum-backed field access
(`stage is ShaderStage.Fragment`) lowers to an `icmp eq` against the constant.

---

## Expression Lowering — CodeGenExpr.cpp

The main dispatcher:
```cpp
llvm::Value* lowerExpr(ExprAST* node, llvm::IRBuilder<>& builder,
                        ValueEnv& env, llvm::Module& mod);
```

### Literals

`LiteralExprAST` where `isConst = true` → emit `llvm::Constant*` directly:
- Int / Hex / Binary → `ConstantInt::get(i32, value)`
- Float → `ConstantFP::get(floatTy, value)`
- String / RawString → `builder.CreateGlobalStringPtr(value)` wrapped in the
  `{ i8*, i64 }` string struct
- Bool `true`/`false` → `ConstantInt::get(i1, 0/1)`
- Nil → `insertvalue { T, i1 } undef, i1 0, 1`

### Identifiers

`IdentifierExprAST` → `env.lookup(name)`. If the returned `Value*` is an `alloca`
(a pointer type), emit `CreateLoad` to get the actual value. Whether it is an
alloca is determined by checking `isa<AllocaInst>()`.

### Binary operators

Arithmetic operators choose signed vs unsigned vs float variants based on
`resolvedType` of the operands (read from `node->resolvedType` cast to `TypeAST*`):

- Integer signed: `CreateAdd`, `CreateSub`, `CreateMul`, `CreateSDiv`, `CreateSRem`
- Integer unsigned: `CreateAdd`, `CreateSub`, `CreateMul`, `CreateUDiv`, `CreateURem`
- Float: `CreateFAdd`, `CreateFSub`, `CreateFMul`, `CreateFDiv`, `CreateFRem`
- Power `^`: lower to a call to `llvm.powi.f32` (int exponent) or `llvm.pow.f32`
  (float exponent) intrinsic
- Comparison: `CreateICmpSLT/SGT/...` for signed integers, `CreateFCmpOLT/...`
  for floats; always produces `i1`
- Logical `and`/`or`: short-circuit lowering with two basic blocks each (do not
  evaluate the RHS if the result is already determined from the LHS)
- Bitwise: `CreateAnd`, `CreateOr`, `CreateXor`, `CreateShl`, `CreateAShr`

### Assignments

`AssignExprAST` — the LHS must produce a pointer (an alloca or a GEP). After
lowering the LHS with a flag `asLValue = true`, emit `CreateStore(rhs_val, lhs_ptr)`.
Compound operators (`+=` etc.) desugar exactly as the semantic pass models them:
load LHS, apply the operator, store the result.

### Field access

`FieldAccessExprAST` (`v.x`):
1. Lower `node->object` to get the struct pointer.
2. Look up the struct's `StructDeclAST` from the symbol via `resolvedType`.
3. Find the field's index in `structDecl->fields` by name.
4. Emit `CreateStructGEP(structType, ptr, fieldIndex)` to get a pointer to the field.
5. If the result is used as an rvalue, emit `CreateLoad`.

For enum variant access (`Direction.North`), the object lowers to the enum's
backing integer type constant. The field name is the variant name looked up in the
`EnumDeclAST`.

### Behavior access

`BehaviorAccessExprAST` (`Vec2:normalize`) → `env.lookupFunc("Vec2.normalize")`.
Returns an `llvm::Function*` value (a function pointer). Since `isBehaviorMember`
is true, the result is never stored back via assignment (the semantic pass blocked
that; codegen can assert).

### Calls

`CallExprAST`:
- Regular call: lower callee to a `Function*`, lower each arg, emit `CreateCall`.
- Generic call (`process<int>(x)`): build the `InstKey` from the explicit `genericArgs` on the node (each lowered via `lowerType`), look up the concrete `llvm::Function*` registered in Pass 1 under the mangled instantiation name, emit `CreateCall`. If the instantiation was not recorded in Pass 0 (which cannot happen in a valid program — the semantic pass verified all use sites), emit a diagnostic and return `undef`.
- Curried partial application: allocate a closure struct on the heap, store the supplied arguments in its fields, return `{ fn_ptr, closure_ptr }` (see Closures below).
- `from()` dispatch (`Fahrenheit(boiling)`): look up `"Fahrenheit.from.celsius"` in the function registry by matching the argument type to the entry's parameter type. The semantic pass already resolved the target function; codegen reads `resolvedType` on the `CallExprAST` to confirm the target.

### Pipelines

`PipelineExprAST` lowers step-by-step:
1. Lower `seed` to produce the initial value.
2. For each `PipelineStepAST`:
   - `Ident`: look up the function, emit `CreateCall(fn, { current_val })`.
   - `BehaviorRef`: look up the mangled function `TypeName.method`, emit `CreateCall`.
   - `FieldRef`: GEP the field (must be a non-nullable function pointer), load it,
     emit indirect call.
   - `ArgPack` (`fn(args)!`): look up the function, emit `CreateCall(fn, { current_val, args... })`.
   - `AnonFunc`: lower the anonymous function inline (see Closures below), then call
     it with `current_val`.
3. The result of each step becomes `current_val` for the next.

### Match expressions

`MatchExprAST` lowers as a chain of conditional branches, one per arm, with the
default arm as the final fallback:

```
; For each arm (top to bottom):
  [lower pattern as a bool condition]
  br %matches_arm_N, arm_N_body, next_arm_check

; arm_N_body:
  [lower arm exprs]
  br merge_bb

; default_body:
  [lower default exprs]
  br merge_bb

; merge_bb:
  %result = phi T [ arm_0_val, arm_0_body ], [ arm_1_val, arm_1_body ], ...
                  [ default_val, default_body ]
```

Literal patterns → `icmp eq`. Range patterns → two `icmp` comparisons joined by
`and`. Type patterns on union structs → extract the tag byte, `icmp eq` against
the variant's tag constant. Struct patterns → chain of GEP + `icmp eq` per field.
Bind patterns → always true; introduce the matched value as a named alloca in the
arm's scope. Wildcard → always true, no binding.

### If expression

`IfExprAST` (the `if cond ?? then else else` form) → classic SSA phi node pattern:

```
  %cond = [lower condition]
  br %cond, then_bb, else_bb

then_bb:
  %then_val = [lower thenBranch]
  br merge_bb

else_bb:
  %else_val = [lower elseBranch]
  br merge_bb

merge_bb:
  %result = phi T [ %then_val, then_bb ], [ %else_val, else_bb ]
```

### Type conversions

`TypeConvExprAST`:
- Safe numeric widening: `CreateSIToFP`, `CreateFPToSI`, `CreateIntCast`, etc.
- Safe string conversion (`string(n)`): call the appropriate `luc_runtime` helper.
- Unsafe bit reinterpret (`*float(bits)`): `CreateBitCast`. Only valid inside
  `insideExtern` context; the semantic pass already enforced this.

---

## Statement Lowering — CodeGenStmt.cpp

```cpp
void lowerStmt(StmtAST* node, llvm::IRBuilder<>& builder,
               ValueEnv& env, llvm::Module& mod,
               llvm::Function* currentFn, llvm::Type* returnType);
```

### Block

`BlockStmtAST` → push scope on `ValueEnv`, lower each stmt, pop scope. The
`scopeDepth` field written by the semantic pass can be used as an assertion
(`assert(env.depth() == node->scopeDepth)`).

### Local declarations inside blocks

`DeclStmtAST` dispatches to either `lowerVarDecl` or `lowerLocalFuncDecl`:
- `VarDeclAST` inside a block: `CreateAlloca` in the entry block of the current
  function (to help `mem2reg`), store the initial value if present. Define the
  alloca pointer in `ValueEnv` under the variable name.
- `FuncDeclAST` inside a block: lower as a closure (see Closures). Define the
  closure value in `ValueEnv`.

### If statement

`IfStmtAST` → same three-block pattern as `IfExprAST` but no phi node and no
merge value. The else block is omitted when `elseBranch` is nullptr; the then
block branches directly to the merge block.

Type-narrowing from `is`-expressions (`if x is Circle { ... }`) is handled by
inserting a `CreateBitCast` of the tagged-union payload at the start of the
then-block, defining the narrowed value in `ValueEnv` under the original name for
the duration of that block. The original binding is restored after the block exits.

### Switch statement

`SwitchStmtAST` → `CreateSwitch(subject_val, default_bb, num_cases)`. For each
`SwitchCaseAST`, call `addCase(ConstantInt, case_bb)`. Range cases cannot be
represented by `CreateSwitch`; they are lowered as a chain of `icmp sge / sle`
comparisons with a conditional branch.

### For loop

```
entry_bb:
  %i = alloca iterVarType           ; loop variable
  store lo, %i                      ; range lower bound (or first element index)
  br cond_bb

cond_bb:
  %cur = load %i
  %cmp = [icmp sle/slt %cur, hi]   ; ..' = sle (inclusive), '..<' = slt (exclusive)
  br %cmp, body_bb, exit_bb

body_bb:
  push_loop(exit_bb, cond_bb)       ; for break / continue
  [lower body stmts]
  pop_loop()
  %next = add %cur, step            ; step defaults to 1
  store %next, %i
  br cond_bb

exit_bb:
  [continues here after loop]
```

Collection iteration over arrays and slices uses the index to GEP the element.
The loop variable is defined in `ValueEnv` as the loaded element value per
iteration.

### While / do-while

Standard condition→body back-edge loops. `push_loop`/`pop_loop` on `ValueEnv`
around the body so break/continue target the correct blocks.

### Return

`ReturnStmtAST` → `CreateRet(val)` or `CreateRetVoid()`.

### Break / Continue

`BreakStmtAST` → `CreateBr(env.current_loop_exit())`.
`ContinueStmtAST` → `CreateBr(env.current_loop_continue())`.

### Parallel

`ParallelForStmtAST` and `ParallelBlockStmtAST` are lowered **sequentially** in
Phase 1 of codegen. A stub comment `; TODO: parallel` is emitted. Full parallel
support (using `std::thread` or LLVM's OpenMP intrinsics) is deferred to a later
milestone. The sequential fallback produces correct output for all programs; the
only missing property is performance.

---

## Closures and Currying

Curried functions and anonymous functions that capture variables are both lowered
as closures: a heap-allocated struct containing the captured values, paired with a
function pointer.

### Closure layout

For a function `let add (a int) (b int) int = { return a + b }`:
- Pass 1 emits the inner function `add.__inner(closure_ptr i8*, b i32) i32`.
- When `add(10)` is encountered at runtime (partial application):
  1. Allocate a closure struct `{ i32 a }` via `luc_alloc`.
  2. Store `10` into the `a` field.
  3. Return `{ fn_ptr = add.__inner, closure_ptr }`.
- When the returned closure is called with `(5)`:
  1. Load `closure_ptr`, cast to `{ i32 }*`.
  2. Call `add.__inner(closure_ptr, 5)`.
  3. Inside `add.__inner`, extract `a` from `closure_ptr` and compute `a + b`.

The closure value itself has LLVM type `{ (i8*, i32) i32*, i8* }` — a function
pointer plus a `void*` carrying the captured environment. The function pointer's
first parameter is always `i8*` (the environment pointer) regardless of the
Luc-level signature.

### Anonymous functions without captures

An anonymous function with no captured variables (e.g. used as a pipeline step
`(x int) int { return x * 2 }`) is lowered as a plain `llvm::Function` with a
synthetic name. No closure struct is allocated. The value is simply a function
pointer of the appropriate type.

### `from` entries

Each `FromEntryAST` inside a `FromDeclAST` is lowered identically to a regular
function. The mangled name `TargetType.from.entryName` is registered in Pass 1.
Curried `from` entries follow the same closure strategy as curried functions above.

---

## Runtime Contract — `luc_runtime.c`

A small C file compiled alongside the JIT module provides heap management and
dynamic array operations. These are declared in the Luc program via `extern let`
and linked by the JIT.

```c
// Memory
void* luc_alloc(uint64_t bytes);
void  luc_free(void* ptr);

// Dynamic array operations  (LucArray = { T* ptr, int64 len, int64 cap })
void  luc_array_push  (void* arr, const void* elem, uint64_t elemSize);
void* luc_array_pop   (void* arr, uint64_t elemSize);
void  luc_array_insert(void* arr, int64_t idx, const void* elem, uint64_t elemSize);
void* luc_array_remove(void* arr, int64_t idx, uint64_t elemSize);
void  luc_array_clear (void* arr);
void  luc_array_reserve(void* arr, int64_t n, uint64_t elemSize);

// String helpers
void*   luc_string_concat(const void* a, const void* b);    // returns { i8*, i64 }
int64_t luc_string_len   (const void* s);
void*   luc_int_to_string(int64_t n);
void*   luc_float_to_string(float f);
void*   luc_bool_to_string(int8_t b);

// io (until a full io module exists)
void luc_printl(const void* str);
```

The `LucArray` struct layout used by the runtime matches the LLVM struct
`{ T*, i64, i64 }` — the codegen passes a pointer to the alloca holding that
struct, and the runtime mutates it in place.

---

## JIT Execution

```cpp
void CodeGen::runJIT() {
    // 1. Verify the module — crashes loudly on malformed IR
    llvm::verifyModule(*module_, &llvm::errs());

    // 2. Run optimization passes
    //    Minimum required:   mem2reg  (promotes allocas to SSA registers)
    //    Recommended:        instcombine, simplifycfg
    //    Optional (fast):    inliner (threshold ~250 instructions)
    llvm::legacy::FunctionPassManager fpm(module_.get());
    fpm.add(llvm::createPromoteMemoryToRegisterPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.doInitialization();
    for (auto& fn : *module_) fpm.run(fn);

    // 3. Create LLVM ORC JIT (LLVM 18 — llvm::orc::LLJITBuilder)
    auto jit = llvm::orc::LLJITBuilder().create();

    // 4. Add module + runtime symbols
    jit->addIRModule(llvm::orc::ThreadSafeModule(
        std::move(module_), std::move(context_)));

    // 5. Look up and call main
    auto mainSym = jit->lookup("main");
    auto mainFn  = (int(*)()) mainSym->getValue();
    int exitCode = mainFn();
}
```

The `main` function emitted by codegen has LLVM signature `i32 @main()` — a direct
match to C's `int main()`. The semantic pass guarantees this signature
(`export imt main () int` with no parameters). The JIT loader calls it and the
return value becomes the process exit code.

---

## Generics — Monomorphization

The semantic pass validates generic declarations and use sites but does **not**
instantiate them — it leaves `Vec2<float>` represented as `NamedTypeAST("Vec2")`
with `genericArgs = [Float]`. Codegen is responsible for instantiation.

Luc uses **monomorphization**: for each unique combination of type arguments
encountered in the program, a separate concrete LLVM struct and a separate concrete
LLVM function are emitted. There is no boxing, no `void*` indirection, and no
runtime type dispatch. Every instantiation is a fully specialized, optimized
concrete type — this is mandatory for Vulkan buffer layouts, fixed-size GPU
structs, and systems-level performance.

### Why not type erasure?

Type erasure (one generic function body serving all instantiations via `void*`) is
explicitly wrong for Luc. `[4]float` and `[16]float` have different sizes that must
be exact for GPU memory layout. A `Vec2<float>` and a `Vec2<int>` must be two
distinct struct types with different field LLVM types. Erasure would collapse them.

### The InstKey

Every generic instantiation is identified by an `InstKey`:

```cpp
struct InstKey {
    std::string              baseName;  // "Vec2", "Buffer", "process"
    std::vector<llvm::Type*> typeArgs;  // concrete LLVM types in declaration order
};
```

Two `InstKey`s are equal when `baseName` matches and every element of `typeArgs`
is pointer-equal (same `llvm::Type*` object — LLVM uniquifies types within a
context, so this is safe and fast).

### Pass 0 — Collecting all instantiations

Pass 0 is a pure read-only walk. It visits every expression and type annotation
in every file and records `InstKey`s for each generic use site. The four places
that produce instantiation keys are:

**Generic struct use** — `NamedTypeAST` with non-empty `genericArgs`:
```
Vec2<float>    →  InstKey { "Vec2",    [float] }
Map<string, int> → InstKey { "Map",   [{ i8*, i64 }, i32] }
Buffer<[4]float> → InstKey { "Buffer", [[4 x float]] }
```

**Generic struct literal** — `StructLiteralExprAST` whose struct has generic params:
```
Vec2<float> { x = 1.0  y = 2.0 }  →  InstKey { "Vec2", [float] }
```

**Generic function call** — `CallExprAST` with non-empty `genericArgs`:
```
process<int>(x)     →  InstKey { "process",   [i32] }
sort<float>(arr)    →  InstKey { "sort",       [float] }
```

**Generic impl methods** — when a method is called on a generic struct instance,
the struct's resolved type provides the type arguments:
```
let buf Buffer<int> = ...
buf -> Buffer:push(42)  →  InstKey { "Buffer.push", [i32] }
```

All collected `InstKey`s are stored in `ValueEnv::insts_`. Duplicate keys are
silently ignored — the same instantiation from multiple call sites produces only
one LLVM function.

### Pass 1 — Emitting concrete types and signatures

For a generic `StructDeclAST` (one with non-empty `genericParams`), Pass 1
iterates `env.instsFor(structName)` and for each `InstKey`:

1. Build a `TypeSubst` map from `genericParams[i].name → typeArgs[i]`.
2. Create a new `llvm::StructType` named with the mangled instantiation name.
3. Lower each field type using `lowerType(field->type, ctx, env, subst)`. When
   `lowerType` encounters a `NamedTypeAST` whose name is in `subst`, it returns
   `subst.at(name)` instead of looking up the struct registry.
4. Call `setBody()` with the substituted field types.
5. Register under the mangled name `"Vec2<float>"`.

For a generic `FuncDeclAST` or `MethodDeclAST`, Pass 1 iterates the matching
`InstKey`s and for each:

1. Build the `TypeSubst` map.
2. Lower each parameter type and the return type with the substitution.
3. Create the `llvm::Function` with the mangled instantiation name.
4. Register in `ValueEnv`.

Non-generic declarations are handled exactly as before — the `InstKey` machinery
is transparent to them.

### Pass 2 — Lowering generic bodies

For each generic function instantiation created in Pass 1, Pass 2 lowers the
body **once per instantiation**:

1. Push the `TypeSubst` onto `ValueEnv`'s substitution stack.
2. Declare params in `ValueEnv` using their substituted types.
3. Call `lowerStmt(body, ...)` — every `lowerType` call inside will consult the
   substitution stack and return concrete types for generic params.
4. Pop the substitution stack.

The body AST is the **same node** for all instantiations — the substitution stack
is what makes each lowering produce different IR. The same `FuncDeclAST` for
`process<T>` is lowered twice if both `process<int>` and `process<float>` are
used, producing two separate `llvm::Function` objects with different IR.

### Trait constraints in generics

Trait constraints (`T : Drawable`) are checked by the semantic pass and have no
codegen representation. By the time codegen runs, every type argument has already
been verified to satisfy its constraints. Codegen can ignore `GenericParamAST`
constraints entirely and only use `genericParams[i].name` to build the
`TypeSubst` map.

### Generic type aliases

`type Transform<T> = (value T) T` — type aliases are transparent at codegen time
(the semantic pass resolved them during Phase 2). A `NamedTypeAST` for a type
alias is never seen by codegen because `TypeResolver` unwraps aliases before the
AST leaves the semantic phase.

### Concrete example

```luc
pub struct Vec2<T> {
    x T
    y T
}

pub impl<T> Vec2<T> {
    dot (other Vec2<T>) T = { return x * other.x + y * other.y }
}

let main () int = {
    let a Vec2<float> = Vec2<float> { x = 1.0  y = 0.0 }
    let b Vec2<float> = Vec2<float> { x = 0.0  y = 1.0 }
    let d float = a -> Vec2:dot(b)!
    return 0
}
```

**Pass 0 collects:**
- `InstKey { "Vec2", [float] }` from `Vec2<float>` in the variable declarations and struct literals
- `InstKey { "Vec2<float>.dot", [float] }` from `Vec2:dot` called on a `Vec2<float>` receiver

**Pass 1 emits:**
```llvm
%Vec2<float> = type { float, float }

define float @Vec2<float>.dot(%Vec2<float>* %self, %Vec2<float>* %other) {
  ; body emitted in Pass 2
}
```

**Pass 2 lowers the body** with `subst = { "T" → float }`:
```llvm
define float @Vec2<float>.dot(%Vec2<float>* %self, %Vec2<float>* %other) {
entry:
  %x_self  = getelementptr %Vec2<float>, %Vec2<float>* %self,  i32 0, i32 0
  %y_self  = getelementptr %Vec2<float>, %Vec2<float>* %self,  i32 0, i32 1
  %x_other = getelementptr %Vec2<float>, %Vec2<float>* %other, i32 0, i32 0
  %y_other = getelementptr %Vec2<float>, %Vec2<float>* %other, i32 0, i32 1
  %lx = load float, float* %x_self
  %ly = load float, float* %y_self
  %rx = load float, float* %x_other
  %ry = load float, float* %y_other
  %px = fmul float %lx, %rx
  %py = fmul float %ly, %ry
  %r  = fadd float %px, %py
  ret float %r
}
```

---

## Implementation Order

Each step is independently testable before moving to the next.

1. **`CodeGenType.cpp`** — pure type mapping with substitution map support, no IR. Test by printing LLVM type strings for every `TypeAST` variant including generic param resolution.
2. **`ValueEnv.hpp`** — scope stack, registries, instantiation registry, substitution stack, loop stack. Unit-testable without LLVM.
3. **Pass 1 of `CodeGenDecl.cpp` (non-generic only)** — struct layouts and function signatures for non-generic declarations. Print IR; verify struct field offsets and function signatures.
4. **`CodeGenExpr.cpp` — literals and arithmetic** — enough to lower `return 1 + 2` inside `main`. Run via JIT; check exit code `3`.
5. **Pass 2 of `CodeGenDecl.cpp` + `CodeGenStmt.cpp` — block + return** — run a `main` that returns a literal int.
6. **Variables (alloca + load/store) + assignment** — `let x int = 5; return x`.
7. **If statements and boolean operators** — basic control flow.
8. **For / while / do-while** — loop emission + break/continue.
9. **Calls** — first regular calls, then impl method calls (mangled names).
10. **Structs** — field access, struct literals, deep-copy semantics.
11. **Enums** — constant lookup, `is` checks.
12. **Arrays** — fixed first, then slices, then dynamic (with runtime calls).
13. **Pipelines and match** — the high-value language features.
14. **Closures and curried functions** — the most complex non-generic part.
15. **`from` conversions** — straightforward once closures are done.
16. **Generics — Pass 0 (instantiation collection)** — walk all use sites, populate `InstKey` registry. Test by printing the collected keys for a program using `Vec2<float>`.
17. **Generics — Pass 1 extension** — emit concrete structs and function signatures for each `InstKey`. Test by printing IR for a generic struct instantiation.
18. **Generics — Pass 2 extension** — lower generic function bodies with substitution stack. Test by running a program that uses `Vec2<float>` and calls a generic method.
19. **JIT + optimization passes** — wire everything together, enable `mem2reg`.
20. **Parallel stubs → real parallel** — separate milestone.
21. **Async/await** — separate milestone (requires LLVM coroutine intrinsics).