I'll read the uploaded files and the relevant headers before giving you a thorough analysis.Good. Now I have everything I need for a comprehensive analysis. Let me write it up.Here's my full analysis across the three dimensions you asked for.

---

## 1. Grammar conformance

**TypeResolver — mostly aligned, with gaps**

The resolver handles all the grammar's type forms correctly: primitives, named types with generics, nullable `?`, all three array forms (`[N]T`, `[]T`, `[*]T`), `&T`, `*T`, and function types with qualifiers/curry groups/multi-return. The qualifier resolution in `visit(FuncTypeAST)` correctly maps raw `~identifier` tokens to bitmask bits.

However, there are two grammar-level gaps:

First, `visit(PtrTypeAST)` silently removed the `@extern`-only restriction that the docstring still claims. The comment says raw pointers are "now allowed anywhere", but the grammar and the docstring on `PtrTypeAST` in `TypeAST.hpp` clearly state pointers are only valid in `@extern`-decorated declarations. This is a deliberate rule that got silently disabled — the enforcement needs to come from somewhere.

Second, `getFunctionReturnType()` has a signature mismatch: the `const` qualifier on the parameter name is `FuncTypeAST& typeconst` (typo — `const` is fused to the parameter name), which is a compilation error in strict mode. Also this function accepts a `const SourceLocation*` hint but uses it only for the multi-return error case, not for the failed-resolution case.

**TypeChecker — well aligned**

The checker correctly models:
- `nil` assignable to any nullable type
- `any` as a universal target (boxing)
- `T → T?` implicit wrapping
- Primitive widening along the signed/unsigned/float chains
- `===` only on structs, references, nullable of those
- `==` only on primitives, enums, nullable types
- `from`-block conversion lookup

The one grammar gap: `isValueComparable` returns `false` for pointer types, but the language spec says nil-check equality (`ptr == nil`) must be allowed for `*T`. The checker doesn't distinguish that case.

---

## 2. AST node alignment

**TypeResolver — two structural issues**

The biggest one is in `visit(FuncDeclAST)` and `visit(ImplDeclAST)`. They construct a temporary `FuncTypeAST` via `arena_.make<FuncTypeAST>()`, copy `node.sig` into it, then call `resolveType()` on the temporary. This means qualifier resolution (`rawQualifiers → qualifiers bitmask`) happens on the *copy*, not on `node.sig` itself. The `sig.rawQualifiers` are cleared on the copy but never on the declaration's own `sig`. The declaration's `node.sig.qualifiers` stays `0`, and `node.sig.rawQualifiers` is never cleared. Phase 3 would then re-encounter raw qualifiers and find them unresolved.

The fix is to resolve qualifiers directly on `node.sig` (or copy back the bitmask from the temporary after resolution).

The second issue: `visit(StructDeclAST)` lazily creates `node.selfType` using `arena_.make<NamedTypeAST>()`. This is correct design, but the field is declared `mutable ASTPtr<NamedTypeAST>` — an arena-backed unique_ptr with a no-op deleter. Since `ASTPtr` uses `ASTDeleter` (no-op), assigning into `node.selfType` from `arena_.make<>` is safe. However calling `resolveType(sym->type)` later in `visit(NamedTypeAST)` on a `NamedTypeAST` whose name is the struct itself would recurse infinitely if the struct is referenced from its own field. There's currently no cycle guard.

**TypeChecker — two alignment gaps**

`isValidArrayIndex` and `isValidSliceBound` cast `indexExpr->resolvedType` as `static_cast<TypeAST*>(resolvedType)` — this works because `resolvedType` is `void*` on `BaseAST`, but it bypasses the `isValid()` / `isKnown()` contract. If `resolvedType` is null (Phase 3 hasn't run yet, or the expression failed to type-check), this produces a null pointer that reaches `isIntegerType`, which does null-check it. But the error message would say "got 'Unknown'" rather than "expression type not resolved yet", which is confusing.

`isFromCastable` searches using `SymbolTable::findSymbolsByPrefix` with a string prefix, but from-entries are registered with mangled names that encode the parameter type using `NameMangler::mangleType`. If a from-entry's parameter type is a generic parameter (`isGenericParam == true`), `mangleType` returns `"prim0"` or the generic name, which means the search finds it — but `isAssignable(src, firstParamType)` might return true for an abstract `T`. This could yield false positives in from-castability checks for generic from-entries.

---

## 3. Bugs, warnings, and improvements

**Bugs**

`getFunctionReturnType` has the `typeconst` typo — `FuncTypeAST& typeconst` makes `typeconst` the parameter name with `const` appearing to be part of it. This compiles only because `const` is a keyword and the parser reads it as trailing `const` on the reference (which is illegal on a non-member function parameter). This will fail to compile or silently misbehave depending on the compiler version.

`visit(FuncDeclAST)` creates a `FuncTypeAST` copy and resolves it, then sets `sym->type = resolvedType` — but `resolvedType` points to a temporary arena allocation whose `sig.rawQualifiers` was cleared. If anything later inspects `sym->type->as<FuncTypeAST>()->sig.rawQualifiers`, it sees an empty vector and concludes there are no qualifiers to resolve. The source-of-truth `node.sig` still has them.

In `primitiveWidening`, signed integers widen to unsigned integers (`int* → uint*`). The comment acknowledges this is "for positive literals", but the function is called for all assignments. A variable of type `int` holding `-1` would be considered assignable to `uint32`, which is wrong. This gate should only open at the literal level, not for general expressions.

**Warnings / design issues**

`isAssignable(nullptr, to)` returns `isNullable(to)` — treating a null `from` as `nil`. This is convenient but can mask bugs where a type genuinely failed to resolve and `from` is null for the wrong reason. A debug-mode assertion or log would help distinguish the two cases.

The `getConstantIntValue` function only handles `UnaryOp::Neg` and five binary operators. The grammar allows hex and binary literals with underscore separators, and the cleaning loop strips underscores for decimal — but for hex literals, `strtoll(..., nullptr, 16)` already handles `0x` prefix but the underscore-stripping retry loop uses base 10. A hex literal like `0xFF_FF` would fail the first parse (because `strtoll` stops at `_`) and then be retried with base 10 on the cleaned string `0xFFFF`, which would fail since `0x` is not valid in base 10. The result: constant hex literals with underscores return `false` from `getConstantIntValue`.

`isFromCastable` takes `SymbolTable*` as a raw pointer (nullable) and checks `if (!symbols)`. But `TypeChecker` itself doesn't hold a symbol table — the caller must pass it on every call. This is inconsistent with the rest of the checker's design; it would be cleaner to either hold a `SymbolTable*` member or require a non-nullable reference via a separate method signature.

`unify` has asymmetric behavior: it returns `b` if `a` is assignable to `b`, and `a` if `b` is assignable to `a`. For the case where `int` widens to `float`, this returns the correct wider type (`float`). But for `T → T?` wrapping, it returns `T?` (correct). However the check order means that if both `a` is assignable to `b` and `b` is assignable to `a` (e.g., `int` and `int`), it always returns `b`, which is fine — but this means the "preferred" branch type in match arms is always the second arm encountered, which may be surprising when arm types are reordered by the programmer.

**Improvements**

`isEqual` could short-circuit for the `UnknownTypeAST` case — currently it falls to the default `return false`, but adding an early `if (a->isa<UnknownTypeAST>() || b->isa<UnknownTypeAST>()) return false` makes the intent explicit and avoids comparing unknown nodes structurally.

`TypeResolver::visit(FromDeclAST)` constructs a temporary `FuncTypeAST` inside the loop for each entry and adds `entry->returnType` to `sig.returnTypes` — but it does this by pushing `entry->returnType` (a `TypePtr`) into the copy's `returnTypes`. Since `TypePtr` is `unique_ptr` with arena deleter, this doesn't actually move ownership but it does duplicate the pointer in two `sig.returnTypes` vectors. If either vector outlives the other without the arena, the no-op deleter means no double-free, but it's a confusing ownership pattern. The `FuncTypeAST` copy should not need to carry the return types — the entry already has `entry->returnType` which is already resolved by the preceding `resolveType` call.

`QualifierRegistry::instance().setStringPool()` has an early-return guard: `if (stringPool) return;` — meaning re-initialization is silently skipped. `AttributeRegistry` rebuilds even on re-init. This asymmetry could cause issues in test environments that reset the pool between tests.