# Full Implementation Plan for Simplified Semantic Analysis

## Phase 0: Cleanup & Preparation (Week 1)

### 0.1 Identify Files to Remove

```bash
# Remove old symbol-based system
rm src/semantic/SemanticSymbol.hpp
rm src/semantic/SymbolTable.hpp
rm src/semantic/SymbolTable.cpp
rm src/semantic/NameResolver.hpp      # duplicate
rm src/semantic/NameResolver.cpp      # duplicate

# Remove over-engineered resolver hierarchy
rm -rf src/semantic/resolveType/
# Keep only TypeResolver.hpp/cpp (to be created)

# Remove resolveImports from SemanticAnalyzer (moved to collector)
# Update SemanticAnalyzer.cpp to remove Phase 0
```

### 0.2 Create New Directory Structure

```bash
mkdir -p src/semantic/scope
mkdir -p src/semantic/collector
mkdir -p src/semantic/resolver
mkdir -p src/semantic/checker/decl
mkdir -p src/semantic/checker/stmt
mkdir -p src/semantic/checker/expr
mkdir -p src/semantic/helpers
```

## Phase 1: Core Infrastructure (Week 1-2)

### 1.1 Create `src/semantic/scope/Scope.hpp`

```cpp
#pragma once

#include "ast/DeclAST.hpp"
#include "ast/support/InternedString.hpp"
#include <unordered_map>
#include <deque>
#include <vector>

namespace luc {

/**
 * @brief A single lexical scope (e.g., global, function, block).
 * 
 * Stores AST nodes directly – no separate Symbol struct.
 */
struct Scope {
    // Value namespace: VarDeclAST, FuncDeclAST, ParamAST, FieldDeclAST, MethodDeclAST
    std::unordered_map<uint32_t, DeclAST*> values;
    
    // Type namespace: StructDeclAST, EnumDeclAST, TraitDeclAST, TypeAliasDeclAST
    std::unordered_map<uint32_t, TypeDeclAST*> types;
    
    // Overload sets (only for functions with same name)
    std::unordered_map<uint32_t, std::vector<FuncDeclAST*>> overloads;
    
    void clear();
    bool empty() const;
    size_t size() const;
};

/**
 * @brief Manages nested scopes (stack-based).
 * 
 * Lookup searches from innermost to outermost scope.
 */
class ScopeStack {
public:
    void push();                    // Enter new scope (block, function)
    void pop();                     // Exit current scope
    Scope& current();               // Get current (innermost) scope
    const Scope& current() const;
    
    // Lookup (searches all scopes from innermost to outermost)
    DeclAST* lookupValue(InternedString name);
    TypeDeclAST* lookupType(InternedString name);
    std::vector<FuncDeclAST*>* lookupOverloads(InternedString name);
    
    // Lookup only in current scope (no outer search)
    DeclAST* lookupLocalValue(InternedString name);
    TypeDeclAST* lookupLocalType(InternedString name);
    
    // Declaration (always in current scope)
    void declareValue(DeclAST* decl);
    void declareType(TypeDeclAST* decl);
    void declareOverload(FuncDeclAST* func);  // Adds to overload set
    
    // Debugging
    void dump(const StringPool& pool) const;
    size_t depth() const { return scopes_.size(); }
    
private:
    std::deque<Scope> scopes_;
    
    // Helper to find overload set in any scope (for declareOverload)
    std::vector<FuncDeclAST*>* findOverloadSet(InternedString name);
};

} // namespace luc
```

### 1.2 Create `src/semantic/scope/Scope.cpp`

Implement all methods. Key logic:

```cpp
void ScopeStack::declareOverload(FuncDeclAST* func) {
    auto* existingSet = findOverloadSet(func->name);
    if (existingSet) {
        // Check for duplicate signature
        if (hasConflictingSignature(func, *existingSet)) {
            diagnostic::error(...); // Duplicate overload
            return;
        }
        existingSet->push_back(func);
    } else {
        // First function with this name
        current().overloads[func->name.id] = {func};
    }
    current().values[func->name.id] = func;
}
```

### 1.3 Create `src/semantic/helpers/SemanticContext.hpp`

Simplified context:

```cpp
#pragma once

#include "ast/support/ASTArena.hpp"
#include "ast/support/StringPool.hpp"
#include "semantic/scope/Scope.hpp"

namespace luc {

class TypeResolver;  // Forward declaration

struct SemanticContext {
    StringPool& pool;
    ASTArena& arena;
    ScopeStack& scope;
    TypeResolver* typeResolver = nullptr;
    
    // For trait conformance (built in Phase 2.5)
    std::unordered_map<std::string, std::vector<InternedString>> typeTraits;
    
    // Error reporting helpers
    void error(const SourceLocation& loc, DiagCode code, 
               std::initializer_list<std::string> args = {});
    void warning(const SourceLocation& loc, DiagCode code,
                 std::initializer_list<std::string> args = {});
    void note(const SourceLocation& loc, const std::string& msg);
    
    // Trait checking
    bool implementsTrait(TypeAST* type, InternedString traitName);
    
    // Loop/parallel tracking (for break/continue/parallel checks)
    int loopDepth = 0;
    int parallelDepth = 0;
    void enterLoop() { loopDepth++; }
    void exitLoop() { loopDepth--; }
    void enterParallel() { parallelDepth++; }
    void exitParallel() { parallelDepth--; }
    
    SemanticContext(StringPool& p, ASTArena& a, ScopeStack& s)
        : pool(p), arena(a), scope(s) {}
};

} // namespace luc
```

## Phase 2: Type Resolution (Week 2)

### 2.1 Create `src/semantic/resolver/TypeResolver.hpp`

```cpp
#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/scope/Scope.hpp"

namespace luc {

class TypeResolver {
public:
    explicit TypeResolver(ScopeStack& scope) : scope_(scope) {}
    
    /**
     * @brief Resolves a type annotation to its semantic type.
     * 
     * - Primitive types: return as-is
     * - Named types: lookup in type namespace, unwrap aliases recursively
     * - Composite types (nullable, array, etc.): resolve inner types first
     * - Function types: resolve parameter and return types
     */
    TypeAST* resolve(TypeAST* type);
    
    /**
     * @brief Resolves a type alias to its ultimate underlying type.
     * 
     * Follows alias chains (A = B, B = C, C = int) and caches result.
     */
    TypeAST* resolveAlias(TypeAliasDeclAST* alias);
    
    /**
     * @brief Checks if two types are equal (after alias resolution).
     */
    bool typesEqual(TypeAST* a, TypeAST* b);
    
private:
    ScopeStack& scope_;
    
    TypeAST* resolveNamedType(NamedTypeAST* named);
    TypeAST* resolveNullableType(NullableTypeAST* nullable);
    TypeAST* resolveResultType(ResultTypeAST* result);
    TypeAST* resolveArrayType(ArrayTypeAST* array);
    TypeAST* resolveRefType(RefTypeAST* ref);
    TypeAST* resolvePtrType(PtrTypeAST* ptr);
    TypeAST* resolveFuncType(FuncTypeAST* func);
    
    // Cache for resolved aliases (stored on TypeAliasDeclAST)
    // No separate map needed – we store on the AST node itself
};

} // namespace luc
```

### 2.2 Create `src/semantic/resolver/TypeResolver.cpp`

Implementation is a simple recursive visitor:

```cpp
TypeAST* TypeResolver::resolve(TypeAST* type) {
    if (!type) return nullptr;
    
    switch (type->kind) {
        case ASTKind::PrimitiveType:
            return type;  // Already resolved
            
        case ASTKind::NamedType:
            return resolveNamedType(type->as<NamedTypeAST>());
            
        case ASTKind::NullableType:
            return resolveNullableType(type->as<NullableTypeAST>());
            
        case ASTKind::ResultType:
            return resolveResultType(type->as<ResultTypeAST>());
            
        case ASTKind::ArrayType:
            return resolveArrayType(type->as<ArrayTypeAST>());
            
        case ASTKind::RefType:
            return resolveRefType(type->as<RefTypeAST>());
            
        case ASTKind::PtrType:
            return resolvePtrType(type->as<PtrTypeAST>());
            
        case ASTKind::FuncType:
            return resolveFuncType(type->as<FuncTypeAST>());
            
        default:
            return type;  // Unknown or already resolved
    }
}

TypeAST* TypeResolver::resolveNamedType(NamedTypeAST* named) {
    // Look up in type namespace
    TypeDeclAST* decl = scope_.lookupType(named->name);
    if (!decl) {
        return named;  // Unresolved (will error later)
    }
    
    // Handle type alias (unwrap recursively)
    if (auto* alias = decl->as<TypeAliasDeclAST>()) {
        return resolveAlias(alias);
    }
    
    // For struct/enum/trait, return a reference to the type
    // Create or reuse selfType on the declaration
    if (!decl->selfType) {
        decl->selfType = new NamedTypeAST(decl->name);
    }
    return decl->selfType;
}
```

## Phase 3: Declaration Collection (Week 2-3)

### 3.1 Create `src/semantic/collector/DeclarationCollector.hpp`

```cpp
#pragma once

#include "ast/ProgramAST.hpp"
#include "semantic/scope/Scope.hpp"
#include "semantic/helpers/SemanticContext.hpp"

namespace luc {

/**
 * @brief Phase 1: Collects all declarations into scopes.
 * 
 * Walks all ProgramASTs and registers declarations in the appropriate
 * namespaces (value or type). Handles duplicate detection.
 */
class DeclarationCollector {
public:
    explicit DeclarationCollector(SemanticContext& ctx);
    
    void collect(const std::vector<ProgramAST*>& programs);
    
private:
    SemanticContext& ctx_;
    std::unordered_set<std::string> processedFiles_;
    
    void collectProgram(ProgramAST* program);
    void collectUseDecl(UseDeclAST* use);
    void collectVarDecl(VarDeclAST* var);
    void collectFuncDecl(FuncDeclAST* func);
    void collectStructDecl(StructDeclAST* structDecl);
    void collectEnumDecl(EnumDeclAST* enumDecl);
    void collectTraitDecl(TraitDeclAST* trait);
    void collectImplDecl(ImplDeclAST* impl);
    void collectFromDecl(FromDeclAST* from);
    void collectTypeAliasDecl(TypeAliasDeclAST* alias);
    
    // Helpers
    void registerValue(DeclAST* decl);
    void registerType(TypeDeclAST* decl);
    void registerOverload(FuncDeclAST* func);
    void checkDuplicateUse(const std::string& path, const SourceLocation& loc);
    
    // Track imports per file (for duplicate detection)
    std::unordered_map<InternedString, std::unordered_set<std::string>> fileImports_;
};

} // namespace luc
```

### 3.2 Implementation Notes

- No separate `Symbol` creation – register AST nodes directly.
- For `ImplDecl` and `FromDecl`, store in separate lists (not in scopes).
- For `UseDecl`, track per‑file to detect duplicates (moved from Phase 0).

## Phase 4: Type Checking (Week 3-4)

### 4.1 Create `src/semantic/checker/TypeChecker.hpp`

```cpp
#pragma once

#include "ast/TypeAST.hpp"
#include "semantic/scope/Scope.hpp"

namespace luc {

/**
 * @brief Type compatibility utilities (stateless).
 * 
 * All methods are static – no internal state.
 */
class TypeChecker {
public:
    // Equality (after alias resolution)
    static bool isEqual(TypeAST* a, TypeAST* b, TypeResolver& resolver);
    
    // Assignment compatibility (with implicit conversions)
    static bool isAssignable(TypeAST* source, TypeAST* target, 
                             TypeResolver& resolver, SemanticContext& ctx);
    
    // Numeric promotion (int → float, byte → int, etc.)
    static bool canPromote(TypeAST* from, TypeAST* to, TypeResolver& resolver);
    
    // Implicit conversion via `from` declarations
    static bool canConvert(TypeAST* from, TypeAST* to, 
                           TypeResolver& resolver, SemanticContext& ctx);
    
    // Unify two types (for type inference)
    static TypeAST* unify(TypeAST* a, TypeAST* b, 
                          TypeResolver& resolver, SemanticContext& ctx);
    
    // Check if type is numeric (for arithmetic)
    static bool isNumeric(TypeAST* type, TypeResolver& resolver);
    
    // Check if type is integer (for indexing)
    static bool isInteger(TypeAST* type, TypeResolver& resolver);
    
    // Get constant integer value (for array sizes)
    static std::optional<int64_t> getConstantInt(ExprAST* expr, 
                                                  TypeResolver& resolver);
};

} // namespace luc
```

## Phase 5: Expression & Statement Checkers (Week 4-6)

### 5.1 Create Dispatcher Pattern

```cpp
// src/semantic/checker/expr/ExprChecker.cpp
TypeAST* checkExpr(ExprAST* expr, SemanticContext& ctx) {
    if (expr->resolvedType) return expr->resolvedType;
    
    TypeAST* result = nullptr;
    switch (expr->kind) {
        case ASTKind::LiteralExpr:
            result = checkLiteralExpr(expr->as<LiteralExprAST>(), ctx);
            break;
        case ASTKind::IdentifierExpr:
            result = checkIdentifierExpr(expr->as<IdentifierExprAST>(), ctx);
            break;
        case ASTKind::BinaryExpr:
            result = checkBinaryExpr(expr->as<BinaryExprAST>(), ctx);
            break;
        // ... all other expression kinds
        default:
            ctx.error(expr->loc, DiagCode::E2001, "unknown expression");
            return nullptr;
    }
    
    expr->resolvedType = result;
    return result;
}
```

### 5.2 Key Checker Implementations

**Identifier Checker:**
```cpp
TypeAST* checkIdentifierExpr(IdentifierExprAST* ident, SemanticContext& ctx) {
    // First check value namespace
    DeclAST* decl = ctx.scope.lookupValue(ident->name);
    if (decl) {
        // Return the type of the declaration
        if (auto* var = decl->as<VarDeclAST>()) return var->type;
        if (auto* func = decl->as<FuncDeclAST>()) return func->funcType;
        // ... etc.
    }
    
    // Then check type namespace
    TypeDeclAST* typeDecl = ctx.scope.lookupType(ident->name);
    if (typeDecl) {
        // Type used as value (e.g., `int` as conversion function)
        return typeDecl->selfType;
    }
    
    ctx.error(ident->loc, DiagCode::E2001, "undefined identifier");
    return nullptr;
}
```

**Call Checker:**
```cpp
TypeAST* checkCallExpr(CallExprAST* call, SemanticContext& ctx) {
    TypeAST* calleeType = checkExpr(call->callee, ctx);
    
    // Check if callee is callable
    if (!TypeChecker::isCallable(calleeType, ctx.typeResolver)) {
        ctx.error(call->loc, DiagCode::E2001, "expression is not callable");
        return nullptr;
    }
    
    // Resolve overload if needed
    if (auto* ident = call->callee->as<IdentifierExprAST>()) {
        auto* overloads = ctx.scope.lookupOverloads(ident->name);
        if (overloads && overloads->size() > 1) {
            FuncDeclAST* best = resolveOverload(*overloads, call->args, ctx);
            if (!best) {
                ctx.error(call->loc, DiagCode::E2001, "ambiguous overload");
                return nullptr;
            }
            return best->funcType->returnTypes[0];
        }
    }
    
    // Normal function call
    // ... check arguments match parameters ...
    return getReturnType(calleeType);
}
```

## Phase 6: Integration (Week 6-7)

### 6.1 Update `SemanticAnalyzer`

```cpp
class SemanticAnalyzer {
public:
    bool analyze(std::vector<ProgramAST*>& programs) {
        // Phase 1: Collect declarations
        DeclarationCollector collector(ctx_);
        collector.collect(programs);
        if (diagnostic::hasErrors()) return false;
        
        // Phase 2: Resolve types
        TypeResolver resolver(ctx_.scope);
        ctx_.typeResolver = &resolver;
        resolveAllTypes(programs, resolver);
        if (diagnostic::hasErrors()) return false;
        
        // Phase 2.5: Build trait conformance map
        buildTraitConformanceMap(programs);
        
        // Phase 3: Check bodies
        checkAllDeclarations(programs);
        if (diagnostic::hasErrors()) return false;
        
        // Phase 3.5: Validate main
        validateEntryPoint();
        
        // Phase 4: Annotate
        annotateAll(programs);
        
        return !diagnostic::hasErrors();
    }
    
private:
    ScopeStack scope_;
    SemanticContext ctx_;
    // No separate symbol table or dispatcher
};
```

## Phase 7: Testing & Migration (Week 7-8)

### 7.1 Unit Tests

Create tests for:
- Basic type resolution (primitive, named, array)
- Type alias chains
- Function overloading
- Scope nesting (shadowing)
- Error recovery (undefined symbols, duplicate declarations)

### 7.2 Integration

- Update `main.cpp` to remove `LUC_DEBUG_DUMP_SYMBOL` references (no longer exists)
- Update all existing checkers to use new APIs
- Remove old files incrementally to avoid breaking builds

## File Summary

| New File                                 | Purpose                           |
| ---------------------------------------- | --------------------------------- |
| `scope/Scope.hpp/cpp`                    | Scope stack with AST node storage |
| `helpers/SemanticContext.hpp/cpp`        | Shared state                      |
| `resolver/TypeResolver.hpp/cpp`          | Type annotation resolution        |
| `checker/TypeChecker.hpp/cpp`            | Type compatibility (stateless)    |
| `collector/DeclarationCollector.hpp/cpp` | Phase 1 collector                 |
| `checker/expr/ExprChecker.cpp`           | Expression checking dispatcher    |
| `checker/stmt/StmtChecker.cpp`           | Statement checking dispatcher     |
| `checker/decl/DeclChecker.cpp`           | Declaration checking dispatcher   |

## Removed Files

| File                                   | Reason                             |
| -------------------------------------- | ---------------------------------- |
| `SemanticSymbol.hpp`                   | No separate symbols                |
| `SymbolTable.hpp/cpp`                  | Replaced by Scope                  |
| `NameResolver.hpp/cpp`                 | Duplicate                          |
| `resolveType/` entire directory        | Over-engineered                    |
| `collectors/SemanticCollector.hpp/cpp` | Replaced by DeclarationCollector   |
| `checkType/TypeChecker.hpp/cpp`        | Moved to `checker/TypeChecker.hpp` |

## Estimated Timeline

| Phase                            | Duration    | Key Deliverable                    |
| -------------------------------- | ----------- | ---------------------------------- |
| 0: Cleanup                       | 2 days      | Removed old files                  |
| 1: Core Infrastructure           | 3 days      | Scope, ScopeStack, SemanticContext |
| 2: Type Resolution               | 3 days      | TypeResolver (single file)         |
| 3: Declaration Collection        | 3 days      | DeclarationCollector               |
| 4: Type Checking                 | 4 days      | TypeChecker (compatibility)        |
| 5: Expression/Statement Checkers | 7 days      | All checkers migrated              |
| 6: Integration                   | 3 days      | SemanticAnalyzer updated           |
| 7: Testing                       | 5 days      | Tests pass, no regressions         |
| **Total**                        | **30 days** | **~4 weeks**                       |

This plan reduces code complexity by ~70% while maintaining full functionality. Would you like me to produce the complete code for any specific component?