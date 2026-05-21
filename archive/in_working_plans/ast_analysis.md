# ASTNode Design Analysis and Improvements

This document provides an analysis of the current ASTNode design in the Luc compiler and suggests improvements focusing on memory management, cache locality, access speed, and convenience for semantic phases, while ensuring scalability and alignment with the grammar.

## 1. Memory Management & Cache Locality (The "Bloat" Problem)

**Observation:**
Currently, `BaseAST` is extremely large. A rough calculation of its size on a 64-bit system:
- `vptr` (virtual table pointer): 8 bytes
- `ASTKind kind`: 2 bytes
- `SourceLocation loc`: ~16 bytes (`int`, `int`, `InternedString`)
- `std::optional<DocComment> doc`: ~24 bytes
- `std::vector<AttributePtr> attributes`: 24 bytes
- `void* resolvedType`: 8 bytes
- `bool isBehaviorMember`, `isConst`: 2 bytes
- `int scopeDepth`: 4 bytes
- `uint32_t effectFlags`: 4 bytes
**Total**: ~92-104 bytes (with padding) per node.

This means a simple expression like `1 + 2` allocates over 300 bytes just for the `BaseAST` fields of the three nodes (`Literal`, `Literal`, `BinaryExpr`).

**Suggestions:**

*   **Move `doc` and `attributes` to `DeclAST`:** 
    According to the `LUC_GRAMMAR.md`, attributes (`@aot`, `@extern`) and doc comments are attached to *declarations*, not expressions or statements. Moving these fields from `BaseAST` to `DeclAST` will save ~48 bytes on every non-declaration node (which make up the vast majority of the AST).
*   **Move `resolvedType` to `ExprAST` and `PatternAST`:** 
    Statements (like `if`, `while`) generally do not evaluate to types in this language design. `resolvedType` is primarily an expression and pattern concept. Moving it down the hierarchy saves 8 bytes on statements and declarations.
*   **Optimize `SourceLocation`:** 
    `SourceLocation` stores an `InternedString file`. Since an entire `ProgramAST` (file) shares the same file path, storing it on every node is redundant. Store `file` in `ProgramAST` only, and reduce `SourceLocation` to just a 32-bit integer (e.g., combining line and column, or representing an offset into a `SourceManager`). This shrinks it from 16 bytes to 4 bytes.
*   **Use Arena-Allocated Arrays instead of `std::vector`:**
    Fields like `std::vector<ExprPtr> args` (in `CallExprAST`) allocate their buffer on the system heap, bypassing the `ASTArena` and causing memory fragmentation/cache misses. Instead, use an arena-allocated slice (e.g., `llvm::ArrayRef` or a custom `ArenaSpan<ExprPtr>`) where the array elements are allocated contiguously in the `ASTArena`.

## 2. Access Speed & Virtual Dispatch

**Observation:**
The AST heavily relies on the Visitor pattern with `virtual void accept(ASTVisitor& visitor) = 0;`. This necessitates a `vptr` (8 bytes per node) and causes indirect branches, which can thwart CPU branch prediction and instruction cache locality during fast semantic passes.

**Suggestions:**

*   **LLVM-Style `switch` Dispatch (CRTP Visitor):**
    You already have a beautifully designed `ASTKind` enum intended for LLVM-style `isa<>` and `as<>`. You can remove the `virtual accept` and `vptr` entirely! 
    Instead, write a CRTP (Curiously Recurring Template Pattern) visitor that does a massive `switch (node->kind)` to dispatch to the correct `visit()` function. This is significantly faster for compiler passes because the CPU can optimize jump tables, and it saves 8 bytes of memory per node.

## 3. Convenience & Type Safety for Semantic Phases

**Observation:**
The semantic dependency direction is correct (AST has no dependency on Semantic). However, there is a small missed opportunity for type safety in `BaseAST.hpp`.

**Suggestions:**

*   **Replace `void* resolvedType` with `TypeAST* resolvedType`:**
    The comment says: *"Forward-declared as void* so BaseAST.hpp has zero dependency on TypeAST"*. However, `struct TypeAST;` is *already* forward-declared at line 160 of `BaseAST.hpp`! Therefore, you can safely define it as `TypeAST* resolvedType` without `#include "TypeAST.hpp"`. This eliminates the need for the semantic pass to cast from `void*` to `TypeAST*`, improving convenience and type safety while maintaining the strictly acyclic dependency graph.
*   **Semantic Side-Tables for Rare Data:**
    Fields like `scopeDepth` are only useful during specific phases (like semantic phase 1: name resolution) or for specific nodes. Instead of bloating `BaseAST` with every possible semantic annotation, consider using an external `std::unordered_map<const BaseAST*, int>` (or a dense arena array) scoped only to the phase that needs it. If codegen needs `scopeDepth`, keep it, but evaluate if it's strictly necessary on *every* node.

## 4. Alignment with Grammar Rules

**Observation:**
The current `BaseAST` structure implicitly assumes that any node can have an attribute or doc comment.

**Suggestions:**
*   By moving `doc` and `attributes` to `DeclAST`, the AST structures strictly enforce the grammar rule `top_level_decl := { attribute } actual_decl`. It prevents invalid states (like an `ExprAST` having a doc comment) from even being representable in memory, naturally aligning the in-memory model with the language specification.

## Summary of Proposed BaseAST Refactor

```cpp
// 1. Much smaller, tighter BaseAST (No vptr, no doc, no attributes)
struct BaseAST {
    ASTKind kind;
    uint32_t locOffset;     // Optimized source location
    uint32_t effectFlags;   // Semantic bitmask
    bool isBehaviorMember;
    bool isConst;
    
    // ... no virtual functions ...
};

// 2. ExprAST carries type info
struct ExprAST : BaseAST {
    TypeAST* resolvedType = nullptr; // Type safe forward-decl
};

// 3. DeclAST carries grammar-specific metadata
struct DeclAST : BaseAST {
    std::optional<DocComment> doc;
    ArenaSpan<AttributePtr> attributes; // Arena-friendly array
};
```
These adjustments will yield a massive reduction in memory footprint, resulting in more AST nodes fitting into the CPU cache (L1/L2), drastically accelerating semantic passes 1-4.

Memory Size Benefits (Estimated)
| Node Type      | Before (bytes) | After (bytes)                | Savings |
| -------------- | -------------- | ---------------------------- | ------- |
| BaseAST        | ~96            | ~16                          | 83%     |
| ExprAST        | ~96 + vptr     | ~24 (including resolvedType) | 75%     |
| DeclAST        | ~96 + vptr     | ~48 (doc + attributes span)  | 50%     |
| LiteralExprAST | ~120           | ~32                          | 73%     |
| BinaryExprAST  | ~136           | ~56                          | 59%     |