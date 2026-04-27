# Luc — Codegen Phase (Updated Plan)

> **Scope of this file:** Architecture, file responsibilities, three-pass strategy,
> type lowering, generics (monomorphization), memory model, closure/curry ABI,
> `from` dispatch, `@extern` handling, comparison operators, nil cleanup,
> `main` entry point variants, and the runtime contract.
> This plan reflects the current AST and semantic pass after the heavy refactor.

---

## Overview

The codegen phase translates the fully annotated, semantically-validated AST into
LLVM IR and then hands it to LLVM's ORC JIT engine for execution.

The phase reads these fields stamped onto the AST by the semantic pass:

| Field | Set by | Used by codegen for |
|---|---|---|
| `resolvedType` | Phase 3b (`checkExpr`) | Choosing the right LLVM type and instruction |
| `isBehaviorMember` | Phase 3b + Phase 4 (Annotator) | Blocking reassignment, mangled name lookup |
| `isConst` | Phase 4 (Annotator) | Emitting `llvm::Constant*` instead of alloca/store |
| `scopeDepth` | Phase 3c (`checkBlock`) | Assertion sanity checks during block lowering |
| `isExtern` / `externSymbol` / `callingConv` | Phase 1 (`SemanticCollector`) | Detecting and emitting external function declarations |

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
└── ValueEnv.hpp           # Scope stack, struct/func/from registries,
                           #   loop-exit/continue block stack,
                           #   generic instantiation registry,
                           #   type substitution stack
```

---

## File Responsibilities

| File | Responsibility |
|:---|:---|
| **`CodeGen.hpp/cpp`** | Driver. Owns `LLVMContext`, `llvm::Module`, `llvm::IRBuilder<>`, and the ORC JIT engine. Runs the three lowering passes over all files in the package, then calls `runJIT()`. |
| **`ValueEnv.hpp`** | Scope stack mapping names to `llvm::Value*`. Also holds the struct-type registry, function registry, `from`-entry registry, generic instantiation registry, and the loop-control stack. Mirrors `SymbolTable` in interface. |
| **`CodeGenType.cpp`** | Pure type mapping. `lowerType(TypeAST*, ctx, env, subst)` converts every `TypeAST` variant to the corresponding `llvm::Type*`. No IR is emitted here. |
| **`CodeGenDecl.cpp`** | Three-pass declaration lowering. Pass 0 collects generic instantiations. Pass 1 forward-declares all struct layouts and function signatures (including `@extern` and `from` entries). Pass 2 fills in function bodies. |
| **`CodeGenExpr.cpp`** | Expression lowering. `lowerExpr(ExprAST*, ...)` dispatches on `ASTKind` and returns an `llvm::Value*`. |
| **`CodeGenStmt.cpp`** | Statement lowering. `lowerStmt(StmtAST*, ...)` handles all statement kinds. Manages `loopStack_` on `ValueEnv`. |

---

## The Three-Pass Strategy

### Pass 0 — Generic Instantiation Collection

Walks every `ProgramAST*` without emitting IR. Discovers every unique
`(baseName, [concreteTypeArgs])` combination used in the program.

Records `InstKey`s for:
- `NamedTypeAST` with non-empty `genericArgs` (e.g. `Vec2<float>`)
- `CallExprAST` with non-empty `genericArgs` (e.g. `process<int>(x)`)
- `StructLiteralExprAST` whose struct has generic params

Non-generic declarations are unaffected.

### Pass 1 — Forward Declarations

Iterates all files without emitting function bodies. For each declaration:

1. **Non-generic structs** (`StructDeclAST`): create `llvm::StructType::create`, call `setBody()`. Register in `ValueEnv`.
2. **Generic structs**: for each `InstKey`, emit one concrete `llvm::StructType` with substituted field types. Register under the mangled name (e.g. `"Vec2<float>"`).
3. **Enums** (`EnumDeclAST`): register each variant as a named `llvm::ConstantInt`. Type is `i8` (≤ 255 variants) or `i16`.
4. **Regular functions** (`FuncDeclAST` where `symbol->isExtern == false`): build `llvm::FunctionType`, create `llvm::Function` with internal linkage. Register in `ValueEnv`.
5. **Extern functions** (`FuncDeclAST` where `symbol->isExtern == true`): build `llvm::FunctionType` from the declared params/return, create `llvm::Function` with `ExternalLinkage` using `symbol->externSymbol` as the LLVM name. No body is emitted in Pass 2. The calling convention from `symbol->callingConv` is applied (`llvm::CallingConv::C` for "C", `llvm::CallingConv::X86_StdCall` for "stdcall", etc.).
6. **`from` entries** (`FromDeclAST`): for each `FromEntryAST`, generate a stable mangled name (see `from` section below) and create the `llvm::Function`. Register in the `from`-entry registry.
7. **Generic functions**: for each matching `InstKey`, emit one concrete `llvm::Function` per instantiation.
8. **Impl methods** (`ImplDeclAST` → `MethodDeclAST`): mangled as `"StructName.method"`, follow the same flow as regular functions. The implicit `self` parameter is the first parameter.

After Pass 1 the entire package's type and function surface is visible to the IR builder.

### Pass 2 — Function Bodies

Iterates all files filling in bodies:

1. Retrieve the `llvm::Function*` from Pass 1.
2. Create an `entry` `BasicBlock`, point `IRBuilder` at it.
3. `alloca` every parameter **in the entry block** (required for `mem2reg`), then `store` the incoming parameter value.
4. For generic instantiations, push the type-substitution map onto `ValueEnv`'s substitution stack.
5. Call `lowerStmt(node->body.get(), ...)`.
6. Pop the substitution map if pushed.
7. Insert a defensive terminator if the last `BasicBlock` has none (`CreateRetVoid()` or `CreateRet(getNullValue(retType))`).

**Skip in Pass 2:** any `FuncDeclAST` where `symbol->isExtern == true`.

> **Critical rule:** all `alloca` instructions must be placed in the entry block
> of their function. Allocas elsewhere prevent `mem2reg` from promoting them to
> SSA registers. The standard pattern: in `lowerStmt` for `DeclStmtAST`, emit
> the `CreateAlloca` into a saved entry-block insertion point, then emit the
> `CreateStore` at the current insertion point.

---

## Naming Conventions

| Declaration | Mangled LLVM name |
|---|---|
| `export const main () int` | `main` (no mangling — C ABI) |
| `export const main (args []string) int` | `main` (with argc/argv C ABI, see main section) |
| `let add (a int) (b int) int` | `add` |
| `impl Vec2 { length () float }` | `Vec2.length` |
| `impl Vec2 { dot (other Vec2) float }` | `Vec2.dot` |
| `from Fahrenheit { (c Celsius) Fahrenheit }` | `Fahrenheit.from.Celsius` |
| `from Fahrenheit { (c Celsius)(scale float) Fahrenheit }` | `Fahrenheit.from.Celsius` (first param type only) |
| `struct Vec2<T>` instantiated as `Vec2<float>` | `Vec2<float>` |
| `let process<T> (x T) string` with `int` | `process<int>` |
| `impl<T> Buffer<T> { push (v T) }` with `float` | `Buffer<float>.push<float>` |

---

## `@extern` — External Function Binding

`@extern` is **not a separate AST node**. It is an `AttributeAST` attached to a regular
`FuncDeclAST`. Detection in codegen uses the symbol table flag set by `SemanticCollector`:

```cpp
Symbol* sym = env.lookupSymbol(funcName);
if (sym->isExtern) {
    // emit ExternalLinkage declaration, no body
}
```

The relevant fields on `Symbol`:
- `sym->isExtern` — true when `@extern` was detected
- `sym->externSymbol` — the C symbol name (e.g. `"malloc"`)
- `sym->callingConv` — calling convention string (e.g. `"C"`, `"stdcall"`)

**Pass 1 for extern functions:**
```cpp
auto* fn = llvm::Function::Create(
    funcType,
    llvm::Function::ExternalLinkage,
    sym->externSymbol,   // use the C symbol name, not the Luc name
    &module);
fn->setCallingConv(resolveCallingConv(sym->callingConv));
env.defineFunc(lucName, fn);  // register under the Luc name for call sites
```

**Pass 2:** skip entirely — `if (sym->isExtern) continue;`

Raw pointer types (`*T`) are valid in `@extern` function signatures. `lowerType`
must accept `PtrTypeAST` without error when lowering these signatures. The
`TypeResolver` already gates this at the semantic level; codegen unconditionally
lowers `PtrTypeAST` → `T*`.

---

## `from` Entries — Stable Mangling

`SemanticCollector` currently mangles `from` entries using pointer addresses
(`"Fahrenheit.from.0x1234abcd"`). This address is not reproducible by codegen.

**The stable scheme used in codegen** is: `"TargetType.from.SourceType"` where
`SourceType` is the name of the first parameter's type in the first parameter group.

Examples:
- `from Fahrenheit { (c Celsius) Fahrenheit = ... }` → `"Fahrenheit.from.Celsius"`
- `from Fahrenheit { (c Celsius)(scale float) Fahrenheit = ... }` → `"Fahrenheit.from.Celsius"` (curried, still keyed on first param)
- `from Fahrenheit { (k Kelvin) Fahrenheit = ... }` → `"Fahrenheit.from.Kelvin"`

**How to extract the source type name** in Pass 1 / Pass 2:

```cpp
// entry is a FromEntryAST*
const std::string& sourceTypeName =
    entry->paramGroups[0][0]->type->as<NamedTypeAST>()->name;
// For primitives, use the primitive kind string: "int", "float", etc.
std::string mangledName = node.targetTypeName + ".from." + sourceTypeName;
```

**`from` dispatch at call sites** (`TypeConvExprAST` where target is `NamedTypeAST`):
1. Get the target type name from `TypeConvExprAST::targetType`.
2. Get the source type from `TypeConvExprAST::expr->resolvedType`.
3. Build `"TargetType.from.SourceType"` and look up in the function registry.
4. Emit `CreateCall` with the inner expression as the argument.

Note: `TypeChecker::isFromCastable` and `SemanticCollector` use the pointer-address
mangling for the semantic-phase symbol table, but **codegen uses the stable
type-name mangling** above. The two registries (semantic symbol table vs codegen
function registry) are separate and do not need to agree on the internal key format.

---

## `main` Entry Point — Two Valid Signatures

The semantic pass now accepts two valid `main` signatures:

```luc
export const main () int = { ... }
export const main (args []string) int = { ... }
```

### Fix Required in `SemanticAnalyzer.cpp`

Replace the "must have zero parameters" check (lines 116–126) with:

```cpp
// Valid signatures:
//   ()           — no arguments
//   (args []string) — command-line arguments
bool validSignature = false;

// Check 1: zero params
bool hasNoParams = true;
for (const auto& group : func->paramGroups) {
    if (!group.empty()) { hasNoParams = false; break; }
}
if (hasNoParams) validSignature = true;

// Check 2: single group with one []string param
if (!validSignature &&
    func->paramGroups.size() == 1 &&
    func->paramGroups[0].size() == 1) {
    TypeAST* pt = func->paramGroups[0][0]->type.get();
    if (pt && pt->isa<SliceTypeAST>()) {
        auto* sl = pt->as<SliceTypeAST>();
        if (sl->element && sl->element->isa<PrimitiveTypeAST>()) {
            auto* prim = sl->element->as<PrimitiveTypeAST>();
            if (prim->primitiveKind == PrimitiveKind::String) {
                validSignature = true;
            }
        }
    }
}

if (!validSignature) {
    dc_.error(DiagnosticCategory::Semantic, func->loc, DiagCode::E3007,
              "'main' must have no parameters or exactly one '[]string' parameter");
}
```

### Codegen for the Two Forms

**Zero-param form** — emit directly as C `int main()`:
```llvm
define i32 @main() { ... }
```

**`[]string` form** — the OS passes `argc`/`argv`. Codegen emits C `int main(int, char**)`,
builds the `{ i8**, i64, i64 }` slice struct from them, then calls the Luc body:
```llvm
define i32 @main(i32 %argc, i8** %argv) {
entry:
  ; build []string slice from argc/argv
  %slice = alloca { i8**, i64, i64 }
  %ptr_field = getelementptr { i8**, i64, i64 }, { i8**, i64, i64 }* %slice, i32 0, i32 0
  store i8** %argv, i8*** %ptr_field
  %len_field = getelementptr { i8**, i64, i64 }, { i8**, i64, i64 }* %slice, i32 0, i32 1
  %len = sext i32 %argc to i64
  store i64 %len, i64* %len_field
  %cap_field = getelementptr { i8**, i64, i64 }, { i8**, i64, i64 }* %slice, i32 0, i32 2
  store i64 %len, i64* %cap_field
  %slice_val = load { i8**, i64, i64 }, { i8**, i64, i64 }* %slice
  ; call the Luc body
  %result = call i32 @main.body({ i8**, i64, i64 } %slice_val)
  ret i32 %result
}
```

In practice the "Luc body" is inlined directly — the wrapper and the body are one function.

---

## ValueEnv

```cpp
struct InstKey {
    std::string              baseName;
    std::vector<llvm::Type*> typeArgs;
    bool operator==(const InstKey&) const;
};

using TypeSubst = std::unordered_map<std::string, llvm::Type*>;

class ValueEnv {
public:
    // Scope stack
    void         pushScope();
    void         popScope();
    void         define(const std::string& name, llvm::Value* val);
    llvm::Value* lookup(const std::string& name) const;

    // Struct type registry
    void              defineType(const std::string& name, llvm::StructType* ty);
    llvm::StructType* lookupType(const std::string& name) const;

    // Function registry (regular + extern + methods + from entries)
    void            defineFunc(const std::string& name, llvm::Function* fn);
    llvm::Function* lookupFunc(const std::string& name) const;

    // From-entry registry — keyed by "TargetType.from.SourceType"
    // Separate from the function registry for clarity; both point to the same Function*.
    void            defineFromEntry(const std::string& mangledName, llvm::Function* fn);
    llvm::Function* lookupFromEntry(const std::string& mangledName) const;

    // Generic instantiation registry
    void                         recordInst(const InstKey& key);
    const std::vector<InstKey>&  instsFor(const std::string& baseName) const;
    bool                         hasInst(const InstKey& key) const;

    // Type substitution stack (for generic bodies)
    void            pushSubst(const TypeSubst& subst);
    void            popSubst();
    llvm::Type*     resolveSubst(const std::string& paramName) const;
    bool            inGenericBody() const;

    // Loop control
    void              push_loop(llvm::BasicBlock* exitBB, llvm::BasicBlock* continueBB);
    void              pop_loop();
    llvm::BasicBlock* current_loop_exit()     const;
    llvm::BasicBlock* current_loop_continue() const;

private:
    std::vector<std::unordered_map<std::string, llvm::Value*>>   scopes_;
    std::unordered_map<std::string, llvm::StructType*>           types_;
    std::unordered_map<std::string, llvm::Function*>             funcs_;
    std::unordered_map<std::string, llvm::Function*>             fromEntries_;
    std::map<std::string, std::vector<InstKey>>                  insts_;
    std::vector<TypeSubst>                                       substStack_;
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> loopStack_;
};
```

---

## CodeGenType — TypeAST → llvm::Type*

```cpp
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
| `PrimitiveTypeAST(Float)` | `float` | |
| `PrimitiveTypeAST(Double)` | `double` | |
| `PrimitiveTypeAST(Decimal)` | `{ i64, i64 }` | 128-bit software-emulated |
| `PrimitiveTypeAST(String)` | `{ i8*, i64 }` | heap pointer + byte length |
| `PrimitiveTypeAST(Char)` | `i32` | Unicode codepoint |
| `PrimitiveTypeAST(Any)` | `{ i8*, i64 }` | data pointer + type tag |
| `NamedTypeAST` (generic param, e.g. `T`) | `subst.at("T")` | from substitution map |
| `NamedTypeAST` (non-generic) | `env.lookupType(name)*` | pointer to struct |
| `NamedTypeAST` (generic inst, e.g. `Vec2<float>`) | `env.lookupType("Vec2<float>")*` | mangled name |
| `NullableTypeAST(T)` | `{ T, i1 }` | value + has_value flag |
| `FixedArrayTypeAST([N]T)` | `[N x T]` | stack-allocated |
| `SliceTypeAST([]T)` | `{ T*, i64, i64 }` | ptr + len + cap |
| `DynamicArrayTypeAST([*]T)` | `{ T*, i64, i64 }` | heap ptr + len + cap |
| `RefTypeAST(&T)` | `T*` | LLVM pointer to T |
| `PtrTypeAST(*T)` | `T*` | raw C pointer — same representation, different semantics |
| `FuncTypeAST` | `llvm::FunctionType*` | from params + return; nullable → `{ fn_ptr, i1 }` |
| `UnionTypeAST` | `{ i8, [N x i8] }` | tag byte + payload sized to largest member |

---

## Memory Model

### Structs — Value Semantics, Deep Copy

Every struct assignment is a `memcpy` into a fresh `alloca`. Fields that hold
`&T` copy only the pointer. Fields that hold `[*]T` (dynamic array) or `string`
copy the `{ T*, i64, i64 }` header only — this is a **shallow copy** of the
heap-managed content, matching the grammar's "deep copy" rule for structs: the
struct owns its header, but for heap-managed fields the programmer is expected
to use explicit cloning functions. This is intentional and matches the grammar's
reference-copy rule for owned heap fields described in Value and Reference Semantics.

### Nullable Types and `nil` Assignment — Heap Cleanup

`T?` lowers to `{ T, i1 }`. The `??` operator lowers to:

```
  %has_val = extractvalue { T, i1 } %nullable, 1
  br %has_val, then_bb, else_bb
then_bb:
  %val = extractvalue { T, i1 } %nullable, 0
  br merge_bb
else_bb:
  %fallback = [lower fallback expr]
  br merge_bb
merge_bb:
  %result = phi T [ %val, then_bb ], [ %fallback, else_bb ]
```

A nil literal lowers to `{ undef, i1 0 }` — the value field is undefined, the
flag is false.

**Assigning `nil` to a variable that currently holds a heap-managed type must
free the old allocation first.** This applies to `[*]T`, `string`, and closure
environments held inside a nullable. The pattern:

```cpp
// lowerNilStore(lhs_ptr, lhs_type, builder, env):
//   1. Load the current { T, i1 } value.
//   2. Extract the has_value flag.
//   3. If true (has a value), call luc_free on the heap pointer inside T.
//   4. Store { undef, i1 0 } into lhs_ptr.
```

Heap-managed types require this cleanup: `DynamicArrayTypeAST`, `PrimitiveTypeAST(String)`,
`AnonFuncExprAST` (closure environment). Fixed arrays, slices, primitives, and
plain structs do not need it.

The runtime must expose:
```c
void luc_release_dynarray(void* arr_ptr);  // calls luc_free on arr->ptr if len > 0
void luc_release_string(void* str_ptr);    // calls luc_free on str->ptr if len > 0
void luc_release_closure(void* closure_ptr); // calls luc_free on the environment
```

---

## Expression Lowering — CodeGenExpr.cpp

```cpp
llvm::Value* lowerExpr(ExprAST* node, llvm::IRBuilder<>& builder,
                        ValueEnv& env, llvm::Module& mod);
```

### Literals

`isConst = true` on the node → emit `llvm::Constant*` directly.
- Int / Hex / Binary → `ConstantInt::get(i32, value)`
- Float → `ConstantFP::get(floatTy, value)`
- String / RawString → `CreateGlobalStringPtr(value)` wrapped in `{ i8*, i64 }`
- Bool → `ConstantInt::get(i1, 0/1)`
- Nil → `{ undef, i1 0 }` — must know the target type from context (the `resolvedType`
  of the expression the nil is being assigned to)

### Identifiers

`env.lookup(name)` → if the returned `Value*` is an `AllocaInst*`, emit `CreateLoad`.

### Binary Operators

Arithmetic operators choose instruction variants based on `resolvedType`:
- Signed integer: `CreateAdd`, `CreateSub`, `CreateMul`, `CreateSDiv`, `CreateSRem`
- Unsigned integer: `CreateAdd`, `CreateSub`, `CreateMul`, `CreateUDiv`, `CreateURem`
- Float: `CreateFAdd`, `CreateFSub`, `CreateFMul`, `CreateFDiv`, `CreateFRem`
- Power `^`: call `llvm.powi.f32` (int exponent) or `llvm.pow.f32` (float exponent)
- Bitwise `&&`, `||`, `~^`, `~`, `<<`, `>>`: `CreateAnd`, `CreateOr`, `CreateXor`,
  `CreateShl`, `CreateAShr` / `CreateLShr`
- Logical `and` / `or`: short-circuit lowering — evaluate LHS, branch on result,
  conditionally evaluate RHS, phi-merge

### Comparison Operators

Three distinct cases:

**`==` / `!=` (value equality, `BinaryOp::Eq` / `BinaryOp::Ne`)**

The semantic pass already blocked struct, function, and array types (E3011/E3012/E3013).
Codegen only handles:
- Primitives, enums: `icmp eq` / `icmp ne`
- Nullable `{ T, i1 }` compared to nil literal: extract the `i1` flag, `icmp eq i1 %flag, 0`
- Nullable `{ T, i1 }` compared to another nullable:
  ```
  %flag_a = extractvalue { T, i1 } %a, 1
  %flag_b = extractvalue { T, i1 } %b, 1
  %both_nil  = icmp eq i1 %flag_a, %flag_b   ; true when both nil or both non-nil
  %val_a = extractvalue { T, i1 } %a, 0
  %val_b = extractvalue { T, i1 } %b, 0
  %vals_eq = icmp eq T %val_a, %val_b
  ; result: both_nil AND (both are nil OR vals_eq)
  ; — implemented as phi or select chain
  ```
- Enums: the backing integer is already an `i8` or `i16`; `icmp eq`

**`===` (reference equality, `BinaryOp::RefEq`)**

Converts both sides to integer via `ptrtoint i64`, then `icmp eq i64`:
```cpp
auto* lhsInt = builder.CreatePtrToInt(lhsPtr, builder.getInt64Ty());
auto* rhsInt = builder.CreatePtrToInt(rhsPtr, builder.getInt64Ty());
return builder.CreateICmpEQ(lhsInt, rhsInt);
```

Valid on `RefTypeAST` values (`T*` LLVM pointers) and struct alloca addresses.
The semantic pass guarantees this is never called on primitives.

**`<`, `>`, `<=`, `>=`**

Signed: `CreateICmpSLT/SGT/SLE/SGE`. Unsigned: `CreateICmpULT/...`. Float: `CreateFCmpOLT/...`.
Always produces `i1`.

### Assignments

`AssignExprAST` — the LHS must produce a pointer (alloca or GEP). Lower LHS with
`asLValue = true` flag, emit `CreateStore(rhs_val, lhs_ptr)`.

Before storing `nil` to a nullable lhs that holds a heap-managed type, call
`lowerNilStore` (see nil cleanup section).

Compound operators (`+=` etc.) desugar: load LHS, apply operator, store result.

### Field Access

`FieldAccessExprAST` (`v.x`):
1. Lower `node->object` to get the struct pointer.
2. Look up the struct's `StructDeclAST` from `resolvedType`.
3. Find the field index by name in `structDecl->fields`.
4. Emit `CreateStructGEP(structType, ptr, fieldIndex)`.
5. If rvalue: emit `CreateLoad`.

Enum variant access (`Direction.North`): lower to the named `llvm::ConstantInt`
registered for that variant.

### Behavior Access

`BehaviorAccessExprAST` (`p:offset`) → look up `"StructName.method"` in the
function registry via `env.lookupFunc(actualTypeName + "." + method)`. The
`actualTypeName` comes from `node->typeName` which the semantic pass already
resolved to the struct name. Returns an `llvm::Function*`.

### Calls

`CallExprAST`:
- **Regular call**: look up callee in function registry, lower each arg, emit `CreateCall`.
- **Extern call**: same as regular — the function was registered in Pass 1 with
  `ExternalLinkage`; `CreateCall` works identically.
- **Generic call** (`process<int>(x)`): build `InstKey` from `genericArgs`, look up
  under the mangled instantiation name (e.g. `"process<int>"`).
- **Curried partial application**: allocate a closure struct, store supplied args,
  return `{ fn_ptr, closure_ptr }`.
- **`from` dispatch** (`TypeConvExprAST` where `targetType` is a `NamedTypeAST`):
  see Type Conversions section.
- **Impl method call** (`p:length()`): the callee is a `BehaviorAccessExprAST`;
  the first argument is the receiver `p` (the struct value or pointer).

### Type Conversions

`TypeConvExprAST` — three distinct cases:

1. **Primitive numeric widening** (e.g. `float(x)` where x is int):
   `CreateSIToFP`, `CreateUIToFP`, `CreateFPToSI`, `CreateIntCast`, `CreateFPExt`, etc.
   Chosen by inspecting both the source `resolvedType` and the target `targetType`.

2. **`from` block dispatch** (`TypeConvExprAST` where `targetType` is `NamedTypeAST`
   and the source is a different named type):
   - Extract `targetName = targetType->as<NamedTypeAST>()->name`.
   - Extract `sourceName` from `expr->resolvedType`.
   - Look up `env.lookupFromEntry(targetName + ".from." + sourceName)`.
   - Lower `expr`, emit `CreateCall(fromFn, { expr_val })`.
   - This is how the semantic-pass desugaring (`let m Minutes = s` wrapped in
     `TypeConvExprAST`) actually executes.

3. **`@bitcast` / unsafe `*T(expr)`** (`isUnsafe == true` or intrinsic `"bitcast"`):
   `CreateBitCast(val, targetLLVMType)`.

4. **Enum → int** (`int(direction)`): the enum backing integer is already the right
   type; emit `CreateZExt` or `CreateTrunc` as needed.

5. **String conversion** (`string(n)`, `string(f)`, `string(b)`):
   call `luc_int_to_string`, `luc_float_to_string`, or `luc_bool_to_string`.

### Pipelines

`PipelineExprAST` — step-by-step:
1. Lower `seed` to produce the initial value.
2. For each `PipelineStepAST`:
   - `Ident`: look up function, emit `CreateCall(fn, { current_val })`.
   - `BehaviorRef`: look up `"TypeName.method"`, emit `CreateCall` with receiver.
   - `FieldRef`: GEP the field (must be non-nullable function pointer), load, indirect call.
   - `ArgPack` (`fn(args)!`): look up function, emit `CreateCall(fn, { current_val, args... })`.
   - `AnonFunc`: lower the anon func inline, call it with `current_val`.
3. Result of each step becomes `current_val` for the next.

### Match Expressions

Chain of conditional branches, one per arm, default arm as final fallback:

```
; For each arm:
  [lower pattern as bool condition]
  br %matches, arm_body_bb, next_check_bb

; arm_body_bb:
  [lower arm exprs]
  br merge_bb

; default_body_bb:
  [lower default exprs]
  br merge_bb

; merge_bb:
  %result = phi T [ arm_0_val, arm_0_body ], ..., [ default_val, default_body ]
```

Pattern lowering:
- **Literal** → `icmp eq` against the constant.
- **Range** `..` (inclusive) → `icmp sge lo` AND `icmp sle hi`. Range `..<` (exclusive) → `icmp sge lo` AND `icmp slt hi`.
- **Enum variant** → `icmp eq` against the variant's `ConstantInt`.
- **Type pattern** (`v is Circle`) → extract the union tag byte, `icmp eq` against the variant's tag constant. Introduce the narrowed alloca in the arm's scope.
- **Struct pattern** (`Vec2 { x: 0.0, y }`) → chain of GEP + field comparisons, `and`ed together. Bind fields to allocas in the arm's scope.
- **Bind pattern** (`n`) → always true; load the subject value, store in a fresh alloca named `n`.
- **Wildcard** (`_`) → always true, no binding.

### If Expression

`IfExprAST` (`if cond ?? then else else`) → classic phi node pattern:
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

### Intrinsic Calls

`IntrinsicCallExprAST` — dispatch by `intrinsicName` to the corresponding LLVM intrinsic
or runtime helper. Key cases:

| Luc intrinsic | LLVM / runtime |
|---|---|
| `@sizeof(T)` | `ConstantInt` of `DL.getTypeAllocSize(lowerType(T))` |
| `@alignof(T)` | `ConstantInt` of `DL.getABITypeAlign(lowerType(T))` |
| `@sqrt(x)` | `llvm.sqrt.f32` or `llvm.sqrt.f64` depending on arg type |
| `@abs(x)` | `llvm.abs.iN` (int) or `llvm.fabs.fN` (float) |
| `@min(a,b)` | `llvm.smin/umin/minnum` depending on type |
| `@max(a,b)` | `llvm.smax/umax/maxnum` depending on type |
| `@floor/ceil/round(x)` | `llvm.floor/ceil/round.fN` |
| `@fma(a,b,c)` | `llvm.fma.fN` |
| `@clz/ctz/popcount(x)` | `llvm.ctlz/cttz/ctpop.iN` |
| `@bswap(x)` | `llvm.bswap.iN` |
| `@memcpy(d,s,n)` | `llvm.memcpy` |
| `@memmove(d,s,n)` | `llvm.memmove` |
| `@memset(d,v,n)` | `llvm.memset` |
| `@bitcast(T, x)` | `CreateBitCast(x, lowerType(T))` |
| `@ptrToRef(T, ptr)` | `CreateBitCast(ptr, lowerType(T))` — asserts validity in debug mode |
| `@refToPtr(ref)` | identity (ref is already a pointer in LLVM IR) |
| `@ptrOffset(ptr, n)` | `CreateGEP(elementType, ptr, n)` |
| `@ptrDiff(p1, p2)` | `ptrtoint` both, `sub`, `sdiv` by element size |

`@sizeof` and `@alignof` use LLVM's `DataLayout` to compute byte sizes at IR
emission time — they are compile-time constants (`isConst = true` on the node).

---

## Statement Lowering — CodeGenStmt.cpp

```cpp
void lowerStmt(StmtAST* node, llvm::IRBuilder<>& builder,
               ValueEnv& env, llvm::Module& mod,
               llvm::Function* currentFn, llvm::Type* returnType);
```

### Block

Push scope on `ValueEnv`, lower each stmt, pop scope.
Assert `env.depth() == node->scopeDepth` as a sanity check.

### Local Declarations

`DeclStmtAST`:
- `VarDeclAST` inside a block: `CreateAlloca` **in the entry block** of the current
  function, `CreateStore` at current insertion point. Define the alloca pointer in
  `ValueEnv`.
- `FuncDeclAST` inside a block: lower as a closure (see Closures). Define the
  closure value in `ValueEnv`.

### If Statement

Three-block pattern (then, else, merge). No phi node. Else block omitted when
`elseBranch` is nullptr. Type narrowing from `is`-expressions: `CreateBitCast`
of the union payload at the start of the then-block, define under the original
name for the block's duration.

### Switch Statement

`CreateSwitch(subject_val, default_bb, num_cases)`. For each `SwitchCaseAST`,
`addCase(ConstantInt, case_bb)`. Range cases cannot use `CreateSwitch`; lower
as a chain of `icmp sge / sle` + `br`.

### For Loop

```
entry_bb:
  %i = alloca iterVarType    [in the function's entry block, not here]
  store lo, %i
  br cond_bb
cond_bb:
  %cur = load %i
  %cmp = icmp sle %cur, hi   [or slt for exclusive ..<]
  br %cmp, body_bb, exit_bb
body_bb:
  push_loop(exit_bb, cond_bb)
  [lower body stmts]
  pop_loop()
  %next = add %cur, step
  store %next, %i
  br cond_bb
exit_bb:
```

Collection iteration: use the index to GEP the element. Define the loop variable
in `ValueEnv` as the loaded element value.

### While / Do-While

Standard condition→body back-edge loops. `push_loop`/`pop_loop` around the body.

### Return

`CreateRet(val)` or `CreateRetVoid()`.

### Break / Continue

`CreateBr(env.current_loop_exit())` / `CreateBr(env.current_loop_continue())`.

### Parallel

`ParallelForStmtAST` and `ParallelBlockStmtAST` are lowered **sequentially** in
the initial milestone. A stub comment `; TODO: parallel` is emitted. Full parallel
support is a separate milestone. The sequential fallback produces correct output.

---

## Closures and Currying

Curried functions and anonymous functions that capture variables are lowered as
closures: a heap-allocated struct containing captured values, paired with a function
pointer.

### Closure Layout

For `let add (a int) (b int) int = { return a + b }`:
- Pass 1 emits the inner function `add.__inner(closure_ptr i8*, b i32) i32`.
- When `add(10)` is encountered:
  1. Allocate closure struct `{ i32 a }` via `luc_alloc`.
  2. Store `10` into the `a` field.
  3. Return `{ fn_ptr = add.__inner, closure_ptr }` as a `{ (i8*, i32) i32*, i8* }`.
- When the returned closure is called with `(5)`:
  1. Load `closure_ptr`, cast to `{ i32 }*`.
  2. Call `add.__inner(closure_ptr, 5)`.
  3. Inside, extract `a` from `closure_ptr`, compute `a + b`.

### Anonymous Functions Without Captures

Lowered as a plain `llvm::Function` with a synthetic name. No closure struct.
The value is simply a function pointer.

### `from` Entries

Each `FromEntryAST` is lowered identically to a regular function. Curried `from`
entries follow the same closure strategy as curried functions.

---

## Runtime Contract — `luc_runtime.c`

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

// Nil-assignment cleanup (called before storing nil into heap-managed nullable)
void luc_release_dynarray(void* arr_ptr);   // luc_free(arr->ptr) if len > 0
void luc_release_string  (void* str_ptr);   // luc_free(str->ptr) if len > 0
void luc_release_closure (void* closure_ptr); // luc_free the environment

// String helpers
void*   luc_string_concat   (const void* a, const void* b);
int64_t luc_string_len      (const void* s);
void*   luc_int_to_string   (int64_t n);
void*   luc_float_to_string (float f);
void*   luc_bool_to_string  (int8_t b);

// io (until the full io module exists)
void luc_printl(const void* str);
```

---

## JIT Execution

```cpp
void CodeGen::runJIT() {
    // 1. Verify the module
    llvm::verifyModule(*module_, &llvm::errs());

    // 2. Run optimization passes
    llvm::legacy::FunctionPassManager fpm(module_.get());
    fpm.add(llvm::createPromoteMemoryToRegisterPass()); // mem2reg — required
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.doInitialization();
    for (auto& fn : *module_) fpm.run(fn);

    // 3. Create LLVM ORC JIT
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

---

## Generics — Monomorphization

Luc uses monomorphization: for each unique combination of type arguments a separate
concrete LLVM struct and function are emitted. No boxing, no `void*` indirection.

### InstKey

```cpp
struct InstKey {
    std::string              baseName;  // "Vec2", "Buffer", "process"
    std::vector<llvm::Type*> typeArgs;  // concrete LLVM types in order
};
```

Two `InstKey`s are equal when `baseName` matches and every element of `typeArgs`
is pointer-equal (LLVM uniquifies types within a context).

### Pass 0 — Collecting Instantiations

Visits every expression and type annotation. Records `InstKey`s for:
- `NamedTypeAST` with non-empty `genericArgs`
- `StructLiteralExprAST` whose struct has generic params
- `CallExprAST` with non-empty `genericArgs`
- Generic impl method calls (the receiver's resolved type provides type args)

### Pass 1 — Emitting Concrete Types and Signatures

For each `InstKey` of a generic `StructDeclAST`:
1. Build `TypeSubst` from `genericParams[i].name → typeArgs[i]`.
2. Create named `llvm::StructType`.
3. Lower each field type with the substitution.
4. `setBody()`, register under mangled name.

For each `InstKey` of a generic function:
1. Build `TypeSubst`.
2. Lower each parameter type and return type.
3. Create `llvm::Function`, register.

### Pass 2 — Lowering Generic Bodies

For each instantiation created in Pass 1:
1. Push `TypeSubst` onto `ValueEnv`'s substitution stack.
2. Declare params with substituted types.
3. `lowerStmt(body)`.
4. Pop substitution stack.

Trait constraints are checked by the semantic pass and have no codegen representation.

---

## Implementation Order

Each step is independently testable before moving to the next.

1. **`CodeGenType.cpp`** — pure type mapping with substitution map support, no IR.
   Test by printing LLVM type strings for every `TypeAST` variant.

2. **`ValueEnv.hpp`** — scope stack, all registries, substitution stack, loop stack.
   Unit-testable without LLVM.

3. **Pass 1 — non-generic structs, enums, function signatures, `@extern` functions, `from` entry signatures.**
   Enum variant constants and extern function declarations both go here.
   Print IR; verify struct field offsets, function signatures, and extern declarations.

4. **`CodeGenExpr.cpp` — literals and arithmetic.**
   Enough to lower `return 1 + 2` inside `main`. Run via JIT; check exit code `3`.

5. **Pass 2 + `CodeGenStmt.cpp` — block + return.**
   Run a `main` that returns a literal int.

6. **Variables (alloca + load/store) + assignment.**
   `let x int = 5; return x`.
   Enforce the alloca-in-entry-block rule here — all allocas go in the entry BB.

7. **If statements, boolean operators, and basic match** (literal patterns, range
   patterns, wildcard, default). Both depend only on what is already working.

8. **For / while / do-while** — loop emission + break/continue.

9. **Regular function calls** (non-method, non-extern, non-generic).

10. **Structs** — field access (GEP + load/store), struct literals (field-by-field
    store), deep-copy assignment (memcpy). Enum `is` checks use constants from step 3.

11. **Impl method calls** (mangled `"StructName.method"`, implicit self parameter).
    Depends on step 10.

12. **Struct/type pattern match** — union tag extraction, struct field pattern GEP chains.
    Depends on steps 10–11.

13. **Arrays** — fixed first (`alloca [N x T]`, GEP indexing), then slices (fat pointer
    construction), then dynamic arrays (runtime calls to `luc_array_*`).

14. **Pipelines** (`->`). Depends on steps 9–11.

15. **Closures and curried functions** — closure struct allocation, `add.__inner` pattern,
    partial application return value.

16. **`from` entry bodies** (signatures were forward-declared in step 3; bodies come here).
    `TypeConvExprAST` → `from` dispatch lookup. Depends on step 15 for curried from entries.

17. **Nullable types** — `{ T, i1 }` layout, `??` operator (phi pattern), nil literal,
    `==`/`!=` on nullable types (flag + value comparison), nil assignment cleanup
    (`luc_release_*` calls before storing nil).

18. **`===` reference equality** — `ptrtoint i64` + `icmp eq`.

19. **`main (args []string)` variant** — argc/argv → `{ i8**, i64, i64 }` slice construction
    in the entry block.
    **Also fix `SemanticAnalyzer.cpp` parameter check before this step** — the semantic
    validator must accept `[]string` params on main before codegen can test this path.

20. **Generics — Pass 0 (instantiation collection).** Walk all use sites, populate
    `InstKey` registry. Test by printing collected keys for a program using `Vec2<float>`.

21. **Generics — Pass 1 extension.** Emit concrete structs and function signatures per
    `InstKey`. Test by printing IR for a generic struct instantiation.

22. **Generics — Pass 2 extension.** Lower generic function bodies with substitution stack.
    Test by running a program that uses `Vec2<float>` and calls a generic method.

23. **JIT + optimization passes.** Wire `mem2reg`, `instcombine`, `simplifycfg`.
    Confirm `mem2reg` promotes all locals (requires allocas in entry blocks — enforced in step 6).

24. **Parallel stubs → real parallel** — separate milestone.

25. **Async/await** — separate milestone (requires LLVM coroutine intrinsics).

---

## Known Issues and Design Decisions

### `from` Symbol Table vs Codegen Registry

`SemanticCollector` mangles `from` entries with pointer addresses
(`"Fahrenheit.from.0x1234abcd"`) because it runs before type resolution and cannot
reliably extract the source type name. `TypeChecker::isFromCastable` and
`SemanticDecl::checkVarDecl` use `findSymbolsByPrefix` to scan all entries under
`"TargetType.from."` without caring about the suffix. This works correctly for
the semantic phase.

Codegen uses the stable `"TargetType.from.SourceType"` scheme independently
because it has resolved types available. The two registries never need to be
compared — one is the semantic symbol table (phase 1–4), the other is the codegen
function registry (phase 5). There is no conflict.

### `==` on Aggregate Types

LLVM cannot compare `struct { T, i1 }` values directly with `icmp eq`. The
nullable comparison logic (step 17) must always decompose the aggregate and compare
components individually. Never use `CreateICmpEQ` on an aggregate type.

### `@extern` Calling Conventions

The calling convention string from `symbol->callingConv` must be mapped to
`llvm::CallingConv::ID`:
- `"C"` → `llvm::CallingConv::C`
- `"stdcall"` → `llvm::CallingConv::X86_StdCall`
- Others: add as needed for Vulkan / platform FFI

### Enum Backing Type

The semantic pass assigns integer values to variants in `checkEnumDecl` but does not
store the computed values back on `EnumVariantAST`. Codegen needs to recompute them:
walk `variants` in order, tracking `nextAuto` (starting at 0), using `explicitValue`
when present. This matches the semantic pass logic exactly.