I’ve analyzed `TypeResolver.cpp` and `.hpp`. Here are the inconsistencies, missing pieces, and required updates.

---

## 🔍 Critical Issues

### 1. Function signatures mismatch (header vs. implementation)
- **Header declares:**  
  `void resolveFunctionType(FuncTypeAST& type);`  
  `std::vector<TypeAST*> getFunctionReturnTypes(FuncTypeAST& type);`  
  `TypeAST* getFunctionReturnType(FuncTypeAST& type);`

- **Implementation uses:**  
  `void resolveFunctionType(FuncTypeAST& type, TypeResolver& resolver);`  
  `std::vector<TypeAST*> getFunctionReturnTypes(FuncTypeAST& type, TypeResolver& resolver);`  
  `TypeAST* getFunctionReturnType(FuncTypeAST& type, TypeResolver& resolver, DiagnosticEngine* dc, const SourceLocation* loc);`

**→ Must change implementation to match header** (remove the extra `resolver` parameter; use `this->resolveType`).

### 2. Missing generic parameter stack (single pointer breaks nesting)
Current `genericParams_` is a single pointer. When a generic struct is inside a generic function, inner resolution overwrites outer context.

**→ Add a stack (`std::vector<const std::vector<GenericParamPtr>*> genericParamsStack_`)** and helpers `pushGenericParams`/`popGenericParams`. Update `visit(NamedTypeAST)` to search the whole stack.

### 3. Missing substitution map stack (same issue)
`substitutionMap_` is a single pointer. Nested instantiations (e.g., generic struct inside a generic function) need a stack.

**→ Add a stack (`std::vector<const std::unordered_map<InternedString, TypeAST*>*> substitutionMapStack_`)** with `pushSubstitutionMap`/`popSubstitutionMap`.

### 4. `visit(FuncDeclAST)` does **not** push generic parameters
The function’s own generic parameters are never set, so names like `T` inside the signature are not recognised.

**→ Before creating the temporary `FuncTypeAST`, push `&node.genericParams`; pop after resolution.**

### 5. `visit(TraitRefAST)` is **not implemented**
The header declares it, but there is no implementation. It should resolve the trait name and its generic arguments.

**→ Implement `visit(TraitRefAST& node)`.**

### 6. `resolveImplMethods` declared but never defined
**→ Remove from header** (not used).

---

## 📋 Required Updates – Action List

### A. Update function signatures in `.cpp`
```cpp
// Change to:
void TypeResolver::resolveFunctionType(FuncTypeAST& type) { ... }  // use this->resolveType
std::vector<TypeAST*> TypeResolver::getFunctionReturnTypes(FuncTypeAST& type) { ... }
TypeAST* TypeResolver::getFunctionReturnType(FuncTypeAST& type, DiagnosticEngine* dc, const SourceLocation* loc) { ... }
```

### B. Add stacks and helpers to `.hpp`
```cpp
private:
    void pushGenericParams(const std::vector<GenericParamPtr>* params);
    void popGenericParams();
    bool isGenericParam(InternedString name) const;

    void pushSubstitutionMap(const std::unordered_map<InternedString, TypeAST*>* map);
    void popSubstitutionMap();
    TypeAST* lookupSubstitution(InternedString name) const;

    std::vector<const std::vector<GenericParamPtr>*> genericParamsStack_;
    std::vector<const std::unordered_map<InternedString, TypeAST*>*> substitutionMapStack_;
```

### C. Update `visit(NamedTypeAST)`
- Replace `genericParams_` checks with `isGenericParam()`.
- Replace `substitutionMap_` lookup with `lookupSubstitution()`.

### D. Update `visit(FuncDeclAST)`
```cpp
void TypeResolver::visit(FuncDeclAST& node) {
    pushGenericParams(&node.genericParams);
    // ... create FuncTypeAST, resolve ...
    popGenericParams();
    // ... update symbol ...
}
```

### E. Update `visit(StructDeclAST)`, `visit(TraitDeclAST)`, `visit(ImplDeclAST)`
- Use `pushGenericParams` / `popGenericParams` instead of manually saving/restoring the single pointer.
- In `visit(ImplDeclAST)`, also push substitution map if built.

### F. Implement `visit(TraitRefAST& node)`
```cpp
void TypeResolver::visit(TraitRefAST& node) {
    // Resolve trait name (must be a trait symbol)
    Symbol* sym = symbols_.lookup(node.name);
    if (!sym || sym->kind != SymbolKind::Trait) {
        dc_.error(...);
        resolved_ = nullptr; return;
    }
    // Resolve generic arguments
    for (auto& arg : node.genericArgs) resolveType(arg.get());
    resolved_ = sym->type; // or &node if we store resolved info
}
```

### G. Remove `resolveImplMethods` from `.hpp`

### H. Optional performance improvements
- **Cache resolved `NamedTypeAST`** – Store the resolved symbol pointer in the node to avoid repeated lookups.
- **Avoid temporary `FuncTypeAST` allocation** for functions (resolve signature components directly). Not critical but can reduce arena waste.

---

## ✅ Summary of files to change
| File               | Changes                                                                                                      |
| ------------------ | ------------------------------------------------------------------------------------------------------------ |
| `TypeResolver.hpp` | Add stacks, remove `resolveImplMethods`, adjust signatures                                                   |
| `TypeResolver.cpp` | Implement stack helpers, update all visitor methods, fix function signatures, implement `visit(TraitRefAST)` |

After these changes, `TypeResolver` will correctly handle nested generics, substitution maps, and match the modern AST design with `InternedString` and `ASTArena`.