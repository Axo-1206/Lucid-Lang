Looking at these files, I'll give you a focused analysis of the key issues.

## SemanticSymbol.hpp

**Uninitialized fields bug** — `kind`, `declKw`, `visibility`, `type`, and `decl` have no default initializers. Using a `Symbol` before fully populating it (easy to do) gives UB.

```cpp
struct Symbol {
    SymbolKind kind = SymbolKind::Var;   // add defaults
    DeclKeyword declKw = DeclKeyword::Let;
    Visibility visibility = Visibility::Private;
    TypeAST* type = nullptr;
    BaseAST* decl = nullptr;
    // ...
};
```

**`std::hash<Symbol>` and `std::equal_to<Symbol>` specializations** are dangerous — `Symbol` is a mutable struct used by value in `unordered_map`. These specializations allow code to accidentally put Symbols in sets/maps keyed by value, but the hash/equality only considers `name+kind`, ignoring the other fields. This will produce silent wrong behavior. Remove them or make them explicit-opt-in via a named hasher struct.

---

## SymbolTable.hpp / SymbolTable.cpp

**Missing `findSymbolsByPrefix` implementation** — declared in the header, referenced in `TypeChecker.cpp` (`isFromCastable`), but the `.cpp` only implements the other methods. The linker will fail unless it's defined elsewhere.

**`getGlobalScope()` returns a `static` empty map as fallback** — this is a footgun. Callers like `validateNoDuplicateSymbols` iterate it expecting real data. If called before `pushScope()`, they get the static empty map silently.

**`lookup` returns a raw pointer into `std::unordered_map` inside a `std::vector`** — this is the dangling pointer bug you already documented in `SemanticDecl.cpp`. But the real fix should be at this layer: the SymbolTable API should make the invalidation risk obvious, or use stable storage. Consider `std::deque` for the scope vector (deque doesn't invalidate references on `push_back`) or `std::list`.

```cpp
// Swap to deque — push_back never invalidates existing element references
std::deque<std::unordered_map<uint32_t, Symbol>> scopes_;
```

This eliminates the entire class of bug documented in `checkImplDecl` without requiring re-lookup after every `pushScope`.

**`lookupLocal` doesn't have a `const` overload** — `exists()` casts away const to call `lookup`, which is UB if the object is actually const.

---

## SemanticCollector.cpp

**`extractExternMetadata` accesses `AttributeArgAST` incorrectly** — it calls `attr->args[0]->as<AttributeArgAST>()` but the args are already `ASTPtr<AttributeArgAST>`, so the cast is redundant and will silently fail if the pointer is to a different node kind. Should be a direct dereference: `attr->args[0].get()`.

**`visit(EnumDeclAST)` declares variants in the global scope** — enum variants get declared as top-level symbols (e.g., `North`, `South`). But the grammar says variants are accessed as `Direction.North`. Putting `North` directly in the global scope means it can shadow other symbols, and `lookup("North")` will find the variant even outside enum context. Variants should either be namespaced (`Direction.North`) or stored inside the enum's symbol entry.

**`visit(ImplDeclAST)` uses `std::string` concatenation for mangling** — `pool_.lookup()` returns `std::string_view`, concatenating two `string_view`s with `+` works but creates temporaries. Fine functionally, but worth noting this happens in a hot path during Phase 1. More importantly, the mangled name `"StructName.methodName"` is fragile — if a struct is literally named `"Foo.bar"`, it would collide with a struct `"Foo"` having method `"bar"`. A separator that can't appear in identifiers (e.g., `"::"` or `"\0"`) would be safer.

**`visit(FromDeclAST)` generates non-unique mangled names** — the mangled name `"From_Fahrenheit_param_param"` is built using a fixed `"_param"` suffix per parameter, meaning two different from-entries with different parameter types get the same mangled name and the second `declareSymbol` silently fails (returns false without error). The actual type information must be part of the key.

**`declareSymbol` reports `E3001` ("undeclared identifier") for a duplicate declaration** — that's the wrong error code. Duplicate declarations should be `E3005`.

---

## Summary of Priority Fixes

1. **`std::deque` for scope storage** — eliminates the dangling pointer class entirely
2. **Implement `findSymbolsByPrefix`** — currently a linker error
3. **Fix `EnumVariant` scoping** — variants pollute global namespace
4. **Fix `FromDecl` mangling** — duplicate entries silently dropped  
5. **Add default initializers to `Symbol`** — prevent UB from partial initialization
6. **Fix `E3001` → `E3005` in `declareSymbol`**