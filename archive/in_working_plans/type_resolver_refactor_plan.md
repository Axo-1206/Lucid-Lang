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

## Phase 1: Core Components (no external dependencies)

These components have no dependencies on other resolvers and can be implemented first.

### 1.1 `core/GenericParamHandler.hpp` + `.cpp`

**Dependencies:** None (only uses standard library)
**Estimated lines:** ~80 total

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

### 1.2 `core/TypeCloner.hpp` + `.cpp`

**Dependencies:** `GenericParamHandler`, `ASTArena`
**Estimated lines:** ~200

```cpp
// TypeCloner.hpp
#pragma once
#include "ast/TypeAST.hpp"

class GenericParamHandler;

class TypeCloner {
public:
    TypeCloner(ASTArena& arena, GenericParamHandler& paramHandler);
    
    TypeAST* clone(const TypeAST* type);
    FuncTypeAST* cloneFunc(const FuncTypeAST* src, const SourceLocation& loc);
    TypeAST* cloneWithSubstitution(const TypeAST* type);
    
private:
    // One private helper per type kind
    TypeAST* clonePrimitive(const PrimitiveTypeAST* src);
    TypeAST* cloneNamed(const NamedTypeAST* src);
    TypeAST* cloneNullable(const NullableTypeAST* src);
    TypeAST* cloneResult(const ResultTypeAST* src);
    TypeAST* cloneArray(const ArrayTypeAST* src);
    TypeAST* cloneRef(const RefTypeAST* src);
    TypeAST* clonePtr(const PtrTypeAST* src);
    FuncTypeAST* cloneFuncInternal(FuncTypeAST* dst, const FuncTypeAST* src, const SourceLocation& loc);
    
    ASTArena& arena_;
    GenericParamHandler& paramHandler_;
};
```

### 1.3 `core/ConstraintChecker.hpp` + `.cpp`

**Dependencies:** `SemanticContext`
**Estimated lines:** ~100

```cpp
// ConstraintChecker.hpp
#pragma once
#include "ast/TypeAST.hpp"
#include "ast/support/InternedString.hpp"
#include <unordered_map>
#include <vector>

struct SemanticContext;

class ConstraintChecker {
public:
    explicit ConstraintChecker(SemanticContext& ctx);
    
    bool satisfies(TypeAST* type, const std::vector<InternedString>& requiredTraits) const;
    void setStructTraits(const std::unordered_map<InternedString, std::vector<InternedString>>* map);
    bool isValueType(TypeAST* type) const;
    
private:
    SemanticContext& ctx_;
    const std::unordered_map<InternedString, std::vector<InternedString>>* structTraits_ = nullptr;
};
```

---

## Phase 2: Simple Inline Resolvers (trivial)

These can be header-only and have minimal dependencies.

### 2.1 `primitive/PrimitiveResolver.hpp`

```cpp
#pragma once
#include "ast/TypeAST.hpp"

struct SemanticContext;

class PrimitiveResolver {
public:
    explicit PrimitiveResolver(SemanticContext& ctx) : ctx_(ctx) {}
    
    TypeAST* resolve(PrimitiveTypeAST& node) {
        // Primitive types are self-contained
        return &node;
    }
    
private:
    SemanticContext& ctx_;
};
```

### 2.2 `composite/ResultResolver.hpp`

```cpp
#pragma once
#include "ast/TypeAST.hpp"

struct SemanticContext;

class ResultResolver {
public:
    explicit ResultResolver(SemanticContext& ctx) : ctx_(ctx) {}
    
    TypeAST* resolve(ResultTypeAST& node) {
        if (node.inner && node.inner->isa<ResultTypeAST>()) {
            ctx_.error(node.loc, DiagCode::E2002, "result type cannot nest '!'");
            return nullptr;
        }
        if (node.errorType && node.errorType->isa<ResultTypeAST>()) {
            ctx_.error(node.loc, DiagCode::E2002, "error type cannot carry '!'");
            return nullptr;
        }
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

struct SemanticContext;

class ArrayResolver {
public:
    explicit ArrayResolver(SemanticContext& ctx) : ctx_(ctx) {}
    
    TypeAST* resolve(ArrayTypeAST& node) {
        if (node.arrayKind == ArrayKind::Fixed && node.size == 0) {
            ctx_.error(node.loc, DiagCode::E2002, "fixed array size must be greater than zero");
        }
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

struct SemanticContext;

class RefResolver {
public:
    explicit RefResolver(SemanticContext& ctx) : ctx_(ctx) {}
    
    TypeAST* resolve(RefTypeAST& node) {
        // Reference type resolution is simple
        return &node;
    }
    
private:
    SemanticContext& ctx_;
};
```

### 2.5 `composite/PtrResolver.hpp`

```cpp
#pragma once
#include "ast/TypeAST.hpp"

struct SemanticContext;

class PtrResolver {
public:
    explicit PtrResolver(SemanticContext& ctx) : ctx_(ctx) {}
    
    TypeAST* resolve(PtrTypeAST& node) {
        // Pointer type resolution is simple
        return &node;
    }
    
private:
    SemanticContext& ctx_;
};
```

### 2.6 `decl/TypeAliasResolver.hpp`

```cpp
#pragma once
#include "ast/DeclAST.hpp"

struct SemanticContext;
class GenericParamHandler;

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

struct SemanticContext;
class GenericParamHandler;
class TypeCloner;

class FuncSignatureResolver {
public:
    FuncSignatureResolver(SemanticContext& ctx, GenericParamHandler& paramHandler, TypeCloner& cloner)
        : ctx_(ctx), paramHandler_(paramHandler), cloner_(cloner) {}
    
    void resolve(FuncDeclAST& node);
    
private:
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
    TypeCloner& cloner_;
};
```

### 2.8 `decl/VarResolver.hpp`

```cpp
#pragma once
#include "ast/DeclAST.hpp"

struct SemanticContext;

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
    
} // namespace ResolverHelpers
```

---

## Phase 3: Medium Complexity Resolvers

These depend on Phase 1 components.

### 3.1 `composite/FuncResolver.hpp` + `.cpp`

**Dependencies:** `GenericParamHandler`, `TypeCloner`, `SemanticContext`
**Estimated lines:** ~150

```cpp
// FuncResolver.hpp
#pragma once
#include "ast/TypeAST.hpp"

struct SemanticContext;
class GenericParamHandler;
class TypeCloner;

class FuncResolver {
public:
    FuncResolver(SemanticContext& ctx, GenericParamHandler& paramHandler, TypeCloner& cloner);
    
    TypeAST* resolve(FuncTypeAST& node);
    TypeAST* getReturnType(const FuncTypeAST& type, const SourceLocation* loc);
    std::vector<TypeAST*> getReturnTypes(const FuncTypeAST& type);
    
private:
    void resolveQualifiers(FuncTypeAST& node);
    void resolveParameters(FuncTypeAST& node);
    void resolveReturnTypes(FuncTypeAST& node);
    
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
    TypeCloner& cloner_;
};
```

### 3.2 `named/NamedResolver.hpp` + `.cpp`

**Dependencies:** `GenericParamHandler`, `ConstraintChecker`, `SemanticContext`
**Estimated lines:** ~200

```cpp
// NamedResolver.hpp
#pragma once
#include "ast/TypeAST.hpp"

struct SemanticContext;
class GenericParamHandler;
class ConstraintChecker;

class NamedResolver {
public:
    NamedResolver(SemanticContext& ctx, GenericParamHandler& paramHandler, ConstraintChecker& constraintChecker);
    
    TypeAST* resolve(NamedTypeAST& node);
    
private:
    TypeAST* resolveGenericParam(NamedTypeAST& node);
    TypeAST* resolveConcreteType(NamedTypeAST& node);
    TypeAST* unwrapTypeAlias(NamedTypeAST& node, Symbol* sym);
    void validateGenericConstraints(NamedTypeAST& node, Symbol* structSym);
    
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
    ConstraintChecker& constraintChecker_;
};
```

### 3.3 `composite/NullableResolver.hpp` + `.cpp`

**Dependencies:** `SemanticContext`, `FuncResolver` (for validation)
**Estimated lines:** ~80

```cpp
// NullableResolver.hpp
#pragma once
#include "ast/TypeAST.hpp"

struct SemanticContext;
class FuncResolver;

class NullableResolver {
public:
    NullableResolver(SemanticContext& ctx, FuncResolver& funcResolver);
    
    TypeAST* resolve(NullableTypeAST& node);
    
private:
    SemanticContext& ctx_;
    FuncResolver& funcResolver_;
};
```

### 3.4 `decl/StructResolver.hpp` + `.cpp`

**Dependencies:** `GenericParamHandler`, `TypeCloner`, `SemanticContext`
**Estimated lines:** ~100

```cpp
// StructResolver.hpp
#pragma once
#include "ast/DeclAST.hpp"

struct SemanticContext;
class GenericParamHandler;
class TypeCloner;

class StructResolver {
public:
    StructResolver(SemanticContext& ctx, GenericParamHandler& paramHandler, TypeCloner& cloner);
    
    void resolve(StructDeclAST& node);
    
private:
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
    TypeCloner& cloner_;
};
```

---

## Phase 4: High Complexity Components

These depend on multiple Phase 2-3 components.

### 4.1 `callable/CallableExtractor.hpp` + `.cpp`

**Dependencies:** `SemanticContext`, `TypeCloner`
**Estimated lines:** ~250

```cpp
// CallableExtractor.hpp
#pragma once
#include "ast/ExprAST.hpp"
#include "ast/TypeAST.hpp"

struct SemanticContext;
class TypeCloner;

class CallableExtractor {
public:
    CallableExtractor(SemanticContext& ctx, TypeCloner& cloner);
    
    FuncTypeAST* extract(ExprPtr& callable, ArenaSpan<TypePtr>& explicitTypeArgs, const SourceLocation& loc);
    TypeAST* resolveReference(ExprPtr& ref, ArenaSpan<TypePtr>& typeArgs, const SourceLocation& loc);
    
private:
    FuncTypeAST* extractFromIdentifier(IdentifierExprAST* ident, const SourceLocation& loc);
    FuncTypeAST* extractFromFieldAccess(FieldAccessExprAST* field, const SourceLocation& loc);
    FuncTypeAST* extractFromCallableRef(CallableRefExprAST* callableRef, const SourceLocation& loc);
    FuncTypeAST* extractFromBehaviorAccess(BehaviorAccessExprAST* behavior, const SourceLocation& loc);
    
    SemanticContext& ctx_;
    TypeCloner& cloner_;
};
```

### 4.2 `injection/InjectionTransformer.hpp` + `.cpp`

**Dependencies:** `SemanticContext`, `TypeCloner`
**Estimated lines:** ~120

```cpp
// InjectionTransformer.hpp
#pragma once
#include "ast/TypeAST.hpp"

struct SemanticContext;
class TypeCloner;

class InjectionTransformer {
public:
    InjectionTransformer(SemanticContext& ctx, TypeCloner& cloner);
    
    FuncTypeAST* transform(FuncTypeAST* funcType, InternedString receiverName, const SourceLocation& loc);
    
private:
    bool validateTransformable(const FuncTypeAST* funcType, const SourceLocation& loc);
    FuncTypeAST* createTransformedType(const FuncTypeAST* src, const SourceLocation& loc);
    
    SemanticContext& ctx_;
    TypeCloner& cloner_;
};
```

### 4.3 `decl/FromResolver.hpp` + `.cpp`

**Dependencies:** `GenericParamHandler`, `CallableExtractor`, `SemanticContext`
**Estimated lines:** ~150

```cpp
// FromResolver.hpp
#pragma once
#include "ast/DeclAST.hpp"

struct SemanticContext;
class GenericParamHandler;
class CallableExtractor;

class FromResolver {
public:
    FromResolver(SemanticContext& ctx, GenericParamHandler& paramHandler, CallableExtractor& callableExtractor);
    
    void resolve(FromDeclAST& node);
    
private:
    void resolveInlineEntry(FromEntryAST& entry, TypeAST* targetType);
    void resolvePathEntry(FromEntryAST& entry, TypeAST* targetType);
    bool validateReturnType(FromEntryAST& entry, TypeAST* targetType);
    
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
    CallableExtractor& callableExtractor_;
};
```

### 4.4 `decl/ImplResolver.hpp` + `.cpp`

**Dependencies:** `GenericParamHandler`, `TypeCloner`, `InjectionTransformer`, `CallableExtractor`, `SemanticContext`
**Estimated lines:** ~300

```cpp
// ImplResolver.hpp
#pragma once
#include "ast/DeclAST.hpp"

struct SemanticContext;
class GenericParamHandler;
class TypeCloner;
class InjectionTransformer;
class CallableExtractor;

class ImplResolver {
public:
    ImplResolver(SemanticContext& ctx,
                 GenericParamHandler& paramHandler,
                 TypeCloner& cloner,
                 InjectionTransformer& injector,
                 CallableExtractor& callableExtractor);
    
    void resolve(ImplDeclAST& node);
    
private:
    TypeAST* resolveTargetType(ImplDeclAST& node);
    TypeAST* unwrapTargetType(TypeAST* target);
    void validateGenericArity(ImplDeclAST& node, TypeAST* underlying, bool isGenericStruct);
    void buildSubstitutionMap(ImplDeclAST& node, TypeAST* target, const ArenaSpan<GenericParamPtr>* targetGenericParams);
    void resolveMethods(ImplDeclAST& node, const std::string& typeName);
    void resolveMethodInline(MethodDeclAST& method);
    void resolveMethodPlainAssignment(MethodDeclAST& method);
    void resolveMethodInjectionAssignment(MethodDeclAST& method);
    
    SemanticContext& ctx_;
    GenericParamHandler& paramHandler_;
    TypeCloner& cloner_;
    InjectionTransformer& injector_;
    CallableExtractor& callableExtractor_;
};
```

---

## Phase 5: TypeDispatcher (Orchestrator)

### 5.1 `TypeDispatcher.hpp` + `.cpp`

**Dependencies:** All of the above
**Estimated lines:** ~200

```cpp
// TypeDispatcher.hpp
#pragma once
#include "ast/TypeAST.hpp"
#include "ast/DeclAST.hpp"
#include "core/GenericParamHandler.hpp"
#include "core/TypeCloner.hpp"
#include "core/ConstraintChecker.hpp"
#include "injection/InjectionTransformer.hpp"
#include "callable/CallableExtractor.hpp"
#include "primitive/PrimitiveResolver.hpp"
#include "named/NamedResolver.hpp"
#include "composite/NullableResolver.hpp"
#include "composite/ResultResolver.hpp"
#include "composite/ArrayResolver.hpp"
#include "composite/RefResolver.hpp"
#include "composite/PtrResolver.hpp"
#include "composite/FuncResolver.hpp"
#include "decl/TypeAliasResolver.hpp"
#include "decl/StructResolver.hpp"
#include "decl/FuncSignatureResolver.hpp"
#include "decl/ImplResolver.hpp"
#include "decl/FromResolver.hpp"
#include "decl/VarResolver.hpp"

struct SemanticContext;

class TypeDispatcher {
public:
    explicit TypeDispatcher(SemanticContext& ctx);
    
    // Main entry point
    TypeAST* resolveType(TypeAST* typeNode);
    
    // Declaration-level resolution
    void resolveTypeAlias(TypeAliasDeclAST& node);
    void resolveStructFields(StructDeclAST& node);
    void resolveFunctionSignature(FuncDeclAST& node);
    void resolveImplMethods(ImplDeclAST& node);
    void resolveFromEntries(FromDeclAST& node);
    void resolveVarType(VarDeclAST& node);
    
    // Access to components (for checkers that need late resolution)
    GenericParamHandler& genericParams() { return genericParams_; }
    ConstraintChecker& constraintChecker() { return constraintChecker_; }
    TypeCloner& cloner() { return cloner_; }
    
private:
    SemanticContext& ctx_;
    
    // Core components
    GenericParamHandler genericParams_;
    TypeCloner cloner_;
    ConstraintChecker constraintChecker_;
    
    // Transformers
    InjectionTransformer injectionTransformer_;
    CallableExtractor callableExtractor_;
    
    // Type resolvers
    PrimitiveResolver primitiveResolver_;
    NamedResolver namedResolver_;
    NullableResolver nullableResolver_;
    ResultResolver resultResolver_;
    ArrayResolver arrayResolver_;
    RefResolver refResolver_;
    PtrResolver ptrResolver_;
    FuncResolver funcResolver_;
    
    // Declaration resolvers
    TypeAliasResolver typeAliasResolver_;
    StructResolver structResolver_;
    FuncSignatureResolver funcSignatureResolver_;
    ImplResolver implResolver_;
    FromResolver fromResolver_;
    VarResolver varResolver_;
};
```

---

## Phase 6: Integration

### 6.1 Update `SemanticContext.hpp`

Change from `TypeResolver*` to `TypeDispatcher*`:

```cpp
// Before
TypeResolver* resolver;

// After
TypeDispatcher* dispatcher;
```

### 6.2 Update `SemanticAnalyzer.cpp`

Update construction and usage:

```cpp
// Before
TypeResolver resolver_(ctx_);
ctx_.resolver = &resolver_;

// After
TypeDispatcher dispatcher_(ctx_);
ctx_.dispatcher = &dispatcher_;
```

### 6.3 Update `checkers/` files

Replace calls to `ctx_.resolver->resolveType()` with `ctx_.dispatcher->resolveType()`

### 6.4 Delete old files

```bash
rm src/semantic/resolveType/TypeResolver.hpp
rm src/semantic/resolveType/TypeResolver.cpp
```

---

## Implementation Order Summary Table

| Phase   | Folder/File                      | Dependencies      | Est. Lines | Priority |
| ------- | -------------------------------- | ----------------- | ---------- | -------- |
| 1.1     | `core/GenericParamHandler`       | None              | 80         | Critical |
| 1.2     | `core/TypeCloner`                | Phase 1.1         | 200        | Critical |
| 1.3     | `core/ConstraintChecker`         | None              | 100        | Critical |
| 2.1-2.9 | Simple inline resolvers          | Phase 1           | 300        | High     |
| 3.1     | `composite/FuncResolver`         | Phase 1           | 150        | High     |
| 3.2     | `named/NamedResolver`            | Phase 1           | 200        | High     |
| 3.3     | `composite/NullableResolver`     | Phase 3.1         | 80         | Medium   |
| 3.4     | `decl/StructResolver`            | Phase 1           | 100        | Medium   |
| 4.1     | `callable/CallableExtractor`     | Phase 1, 2        | 250        | Medium   |
| 4.2     | `injection/InjectionTransformer` | Phase 1, 2        | 120        | Medium   |
| 4.3     | `decl/FromResolver`              | Phase 1, 4.1      | 150        | Medium   |
| 4.4     | `decl/ImplResolver`              | Phase 1, 4.1, 4.2 | 300        | Medium   |
| 5.1     | `TypeDispatcher`                 | All above         | 200        | Final    |
| 6       | Integration                      | All               | N/A        | Final    |

## Testing Strategy per Phase

After each phase, you can run the existing test suite to verify no regression:

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

This is a significant refactoring. I recommend implementing Phase 1 completely first, testing, then moving to Phase 2, etc. Each phase should be committed separately with clear commit messages.