# TypeResolver Refactoring Implementation Order

Based on dependency analysis, here is the recommended order of implementation. Each phase builds on the previous, and you can test incrementally.

## Phase 0: Setup (5 minutes)

Create the directory structure:

```bash
cd src/semantic/resolveType

mkdir -p core
mkdir -p primitive
mkdir -p named
mkdir -p composite
mkdir -p decl
mkdir -p injection
mkdir -p callable
mkdir -p helpers
```

Also ensure `src/semantic/checkType/` exists for `TypeChecker` (already moved per your note).

---

## Phase 1: Core Components

These components have minimal dependencies.

### 1.1 `core/GenericParamHandler.hpp` + `.cpp` — **Class (stateful)**

```cpp
// GenericParamHandler.hpp
#pragma once
#include "ast/BaseAST.hpp"
#include "ast/DeclAST.hpp"
#include <unordered_map>
#include <vector>

class GenericParamHandler {
public:
    GenericParamHandler() = default;
    
    void pushParams(const ArenaSpan<GenericParamPtr>* params);
    void popParams();
    bool isParam(InternedString name) const;
    const ArenaSpan<GenericParamPtr>* currentParams() const;
    
    void pushSubstMap(const std::unordered_map<InternedString, TypeAST*>* map);
    void popSubstMap();
    TypeAST* lookupSubst(InternedString name) const;
    
    void clear();
    
private:
    std::vector<const ArenaSpan<GenericParamPtr>*> paramsStack_;
    std::vector<const std::unordered_map<InternedString, TypeAST*>*> substStack_;
};
```

### 1.2 `core/TypeCloner.hpp` + `.cpp` — **Namespace (stateless)**

```cpp
// TypeCloner.hpp
#pragma once
#include "ast/TypeAST.hpp"
#include "ast/support/ASTArena.hpp"

class GenericParamHandler;

namespace TypeCloner {
    TypeAST* clone(ASTArena& arena, const TypeAST* type);
    FuncTypeAST* cloneFunc(ASTArena& arena, const FuncTypeAST* src);
    TypeAST* cloneWithSubstitution(ASTArena& arena, GenericParamHandler& paramHandler, const TypeAST* type);
    FuncTypeAST* createFuncType(ASTArena& arena, const SourceLocation& loc = SourceLocation());
}
```

### 1.3 `core/ConstraintChecker.hpp` + `.cpp` — **Namespace (stateless)**

```cpp
// ConstraintChecker.hpp
#pragma once
#include "ast/TypeAST.hpp"
#include "ast/support/InternedString.hpp"
#include <vector>

struct SemanticContext;

namespace ConstraintChecker {
    bool satisfies(SemanticContext& ctx, TypeAST* type, const std::vector<InternedString>& requiredTraits);
    bool isValueType(SemanticContext& ctx, TypeAST* type);
    bool isStructType(SemanticContext& ctx, TypeAST* type);
    bool isEnumType(SemanticContext& ctx, TypeAST* type);
    bool isFunctionType(SemanticContext& ctx, TypeAST* type);
    bool isReferenceType(SemanticContext& ctx, TypeAST* type);
    bool isArrayType(SemanticContext& ctx, TypeAST* type);
    bool isValidImplTarget(SemanticContext& ctx, TypeAST* type);
    InternedString getTypeName(SemanticContext& ctx, TypeAST* type);
    TypeAST* unwrapAliases(SemanticContext& ctx, TypeAST* type);
}
```

---

## Phase 2: Simple Inline Resolvers (header-only)

These rely on Phase 1 core components but are trivial.

### 2.1 `primitive/PrimitiveResolver.hpp`

```cpp
#pragma once
#include "ast/TypeAST.hpp"

struct SemanticContext;

class PrimitiveResolver {
public:
    explicit PrimitiveResolver(SemanticContext& ctx) : ctx_(ctx) {}
    TypeAST* resolve(PrimitiveTypeAST& node) { return &node; }
private:
    SemanticContext& ctx_;
};
```

### 2.2 `composite/ResultResolver.hpp`

```cpp
#pragma once
#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

class ResultResolver {
public:
    explicit ResultResolver(SemanticContext& ctx) : ctx_(ctx) {}
    TypeAST* resolve(ResultTypeAST& node) {
        if (node.inner && node.inner->isa<ResultTypeAST>())
            ctx_.error(node.loc, DiagCode::E1021, "result type cannot nest '!'");
        if (node.errorType && node.errorType->isa<ResultTypeAST>())
            ctx_.error(node.loc, DiagCode::E1021, "error type cannot carry '!'");
        return &node;
    }
private:
    SemanticContext& ctx_;
};
```

### 2.3 `composite/ArrayResolver.hpp`

```cpp
#pragma once
#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"
#include "diagnostics/DiagnosticCodes.hpp"

class ArrayResolver {
public:
    explicit ArrayResolver(SemanticContext& ctx) : ctx_(ctx) {}
    TypeAST* resolve(ArrayTypeAST& node) {
        if (node.arrayKind == ArrayKind::Fixed && node.size == 0)
            ctx_.error(node.loc, DiagCode::E2002, "fixed array size must be > 0");
        return &node;
    }
private:
    SemanticContext& ctx_;
};
```

### 2.4 `composite/RefResolver.hpp`

```cpp
#pragma once
#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"

class RefResolver {
public:
    explicit RefResolver(SemanticContext& ctx) : ctx_(ctx) {}
    TypeAST* resolve(RefTypeAST& node) { return &node; }
private:
    SemanticContext& ctx_;
};
```

### 2.5 `composite/PtrResolver.hpp`

```cpp
#pragma once
#include "ast/TypeAST.hpp"
#include "semantic/helpers/SemanticContext.hpp"

class PtrResolver {
public:
    explicit PtrResolver(SemanticContext& ctx) : ctx_(ctx) {}
    TypeAST* resolve(PtrTypeAST& node) { return &node; }
private:
    SemanticContext& ctx_;
};
```

### 2.6 `decl/TypeAliasResolver.hpp`

```cpp
#pragma once
#include "ast/DeclAST.hpp"
#include "core/GenericParamHandler.hpp"

class TypeAliasResolver {
public:
    TypeAliasResolver(SemanticContext& ctx, GenericParamHandler& paramHandler)
        : ctx_(ctx), paramHandler_(paramHandler) {}
    void resolve(TypeAliasDeclAST& node);
private:
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
};
```

### 2.7 `decl/FuncSignatureResolver.hpp`

```cpp
#pragma once
#include "ast/DeclAST.hpp"
#include "core/GenericParamHandler.hpp"
#include "core/TypeCloner.hpp"

class FuncSignatureResolver {
public:
    FuncSignatureResolver(SemanticContext& ctx, GenericParamHandler& paramHandler)
        : ctx_(ctx), paramHandler_(paramHandler) {}
    void resolve(FuncDeclAST& node);
private:
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
};
```

### 2.8 `decl/VarResolver.hpp`

```cpp
#pragma once
#include "ast/DeclAST.hpp"

class VarResolver {
public:
    explicit VarResolver(SemanticContext& ctx) : ctx_(ctx) {}
    void resolve(VarDeclAST& node);
private:
    SemanticContext& ctx_;
};
```

### 2.9 `helpers/ResolverHelpers.hpp`

```cpp
#pragma once
#include "ast/TypeAST.hpp"
#include <string>

struct SemanticContext;

namespace ResolverHelpers {
    bool isPrimitiveType(TypeAST* type);
    bool isStructType(TypeAST* type, const SemanticContext& ctx);
    bool isEnumType(TypeAST* type, const SemanticContext& ctx);
    bool isFunctionType(TypeAST* type);
    bool isReferenceType(TypeAST* type);
    bool isPointerType(TypeAST* type);
    bool isArrayType(TypeAST* type);
    std::string getTypeName(TypeAST* type, const SemanticContext& ctx);
    bool hasGenericParams(TypeAST* type);
    size_t getGenericArity(TypeAST* type);
}
```

---

## Phase 3: Medium Complexity Resolvers (with .cpp files)

These depend on Phase 1 core components (namespaces) and Phase 2 simple resolvers.

### 3.1 `composite/FuncResolver.hpp` + `.cpp`

- Depends on `GenericParamHandler`, `TypeCloner` (namespace)
- Resolves qualifiers, parameters, return types.

### 3.2 `named/NamedResolver.hpp` + `.cpp`

- Depends on `GenericParamHandler`, `ConstraintChecker` (namespace)
- Resolves named types, generic parameters, constraints.

### 3.3 `composite/NullableResolver.hpp` + `.cpp`

- Depends on `SemanticContext`, `FuncResolver` (forward declared)
- Validates nullable types and rule "no `?` on function types".

### 3.4 `decl/StructResolver.hpp` + `.cpp`

- Depends on `GenericParamHandler`, `TypeCloner` (namespace)
- Resolves struct fields and creates self-type.

---

## Phase 4: High Complexity Components

### 4.1 `callable/CallableExtractor.hpp` + `.cpp`

- Uses `TypeCloner` namespace to clone function types from callable AST nodes.
- Resolves identifiers, field accesses, behavior accesses, generic references.

### 4.2 `injection/InjectionTransformer.hpp` + `.cpp`

- Uses `TypeCloner` namespace to remove first parameter from function type for `!` injection.

### 4.3 `decl/FromResolver.hpp` + `.cpp`

- Depends on `GenericParamHandler`, `CallableExtractor`, `TypeCloner` namespace.

### 4.4 `decl/ImplResolver.hpp` + `.cpp`

- Depends on `GenericParamHandler`, `TypeCloner`, `InjectionTransformer`, `CallableExtractor`.
- Validates impl target, generic arity, builds substitution map, resolves methods.

---

## Phase 5: TypeDispatcher (Orchestrator)

### 5.1 `TypeDispatcher.hpp` + `.cpp`

- Owns `GenericParamHandler` instance (stateful).
- Uses `TypeCloner` and `ConstraintChecker` namespaces (no instance needed).
- Owns all resolver class instances.
- Dispatches `resolveType` and declaration-level resolutions.

---

## Phase 6: Integration

### 6.1 Update `SemanticContext.hpp`

- Change `TypeResolver* resolver` → `TypeDispatcher* dispatcher`
- Add `typeTraits` map (key: mangled type, value: list of trait names)

### 6.2 Update `SemanticAnalyzer.cpp`

- Construct `TypeDispatcher` instead of `TypeResolver`
- After Phase 2 type resolution, build `typeTraits` map by iterating impl blocks.

### 6.3 Update `checkers/` files

- Replace `ctx_.resolver->resolveType(...)` with `ctx_.dispatcher->resolveType(...)`

### 6.4 Delete old files

```bash
rm src/semantic/resolveType/TypeResolver.hpp
rm src/semantic/resolveType/TypeResolver.cpp
```

---

## Implementation Order Summary Table

| Phase   | Component               | Type             | Dependencies                  |
| ------- | ----------------------- | ---------------- | ----------------------------- |
| 1.1     | GenericParamHandler     | Class (stateful) | None                          |
| 1.2     | TypeCloner              | Namespace        | ASTArena, GenericParamHandler |
| 1.3     | ConstraintChecker       | Namespace        | SemanticContext, NameMangler  |
| 2.1-2.9 | Simple inline resolvers | Classes          | Phase 1                       |
| 3.1     | FuncResolver            | Class            | Phase 1, 2                    |
| 3.2     | NamedResolver           | Class            | Phase 1, 2                    |
| 3.3     | NullableResolver        | Class            | Phase 3.1                     |
| 3.4     | StructResolver          | Class            | Phase 1, 2                    |
| 4.1     | CallableExtractor       | Class            | Phase 1, 2                    |
| 4.2     | InjectionTransformer    | Class            | Phase 1, 2                    |
| 4.3     | FromResolver            | Class            | Phase 1, 4.1                  |
| 4.4     | ImplResolver            | Class            | Phase 1, 4.1, 4.2             |
| 5.1     | TypeDispatcher          | Class            | All of the above              |
| 6       | Integration             | N/A              | All                           |

## Testing Strategy per Phase

```bash
# After Phase 1 (core components)
make test-core

# After Phase 2 (inline resolvers)
make test-resolvers-simple

# After Phase 3 (medium resolvers)
make test-resolvers-medium

# After Phase 4 (complex components)
make test-resolvers-complex

# After Phase 5 (TypeDispatcher)
make test-type-resolution

# After Phase 6 (full integration)
make test-all
```

## Estimated Total Work

| Phase                       | Estimated Time  |
| --------------------------- | --------------- |
| Phase 0: Setup              | 5 minutes       |
| Phase 1: Core               | 2-3 hours       |
| Phase 2: Simple resolvers   | 1-2 hours       |
| Phase 3: Medium resolvers   | 3-4 hours       |
| Phase 4: Complex components | 4-5 hours       |
| Phase 5: TypeDispatcher     | 1-2 hours       |
| Phase 6: Integration        | 1-2 hours       |
| **Total**                   | **13-18 hours** |

```