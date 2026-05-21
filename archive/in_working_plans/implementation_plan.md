# ASTNode Design Refactoring Plan

We will refactor the ASTNode design in `src/ast/` to optimize memory management (reducing base node bloat, packing source locations) and introduce `ArenaSpan` to replace `std::vector` for arena-allocated contiguous child nodes.

## User Review Required

> [!WARNING]
> **Compilation Breakage Warning:**
> Implementing these changes exclusively inside `src/ast/` (per the instruction *"don't touch other files, only modify what are currently inside the src/ast"*) **will break compilation** of the parser (`src/parser/`), semantic analyser (`src/sem/`), and other compiler phases.
> 
> This is because:
> 1. Changing `std::vector` to `ArenaSpan` removes standard mutating methods like `.push_back()`, `.clear()`, etc.
> 2. Moving `doc`, `attributes`, and `resolvedType` down from `BaseAST` means other phases accessing `node->attributes` or `node->resolvedType` on raw `BaseAST*` or incorrect node categories will fail.
> 3. `SourceLocation` line and column are now accessed via getter methods `.line()` and `.column()` instead of fields `.line` and `.column`.
> 
> We will proceed with the modifications inside `src/ast/` exactly as requested. After you approve, you (or we, in a subsequent instruction) will need to update the parser and semantic phases to align with the new AST contract.

## Proposed Changes

### [AST Support Library]

#### [NEW] [ArenaSpan.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/support/ArenaSpan.hpp)
Create a new header for the lightweight, non-owning `ArenaSpan` template:
```cpp
#pragma once
#include <cstddef>

template <typename T>
class ArenaSpan {
    T*     data_ = nullptr;
    size_t size_ = 0;

public:
    ArenaSpan() = default;
    ArenaSpan(T* data, size_t size) : data_(data), size_(size) {}

    T* data() { return data_; }
    const T* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    T& operator[](size_t idx) { return data_[idx]; }
    const T& operator[](size_t idx) const { return data_[idx]; }

    T* begin() { return data_; }
    T* end()   { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end()   const { return data_ + size_; }
};
```

#### [MODIFY] [ASTArena.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/support/ASTArena.hpp)
- Refactor the allocation logic inside `ASTArena::alloc` to a private helper `allocRaw(size_t size, size_t align)`.
- Implement `allocArray<T>(size_t size)` using `allocRaw`.
- Add `#include "ArenaSpan.hpp"`.

### [AST Node Families]

#### [MODIFY] [BaseAST.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/BaseAST.hpp)
1. **Optimize `SourceLocation`:**
   - Remove `InternedString file;` field.
   - Pack `line` and `column` into a single `uint32_t value;` (20 bits for line, 12 bits for column).
   - Change `line` and `column` accesses to getter functions `.line()` and `.column()`.
2. **Optimize `BaseAST`:**
   - Remove `std::vector<AttributePtr> attributes;` (move to `DeclAST`).
   - Remove `std::optional<DocComment> doc;` (move to `DeclAST`).
   - Change `void* resolvedType` from `BaseAST` (move to `ExprAST` and `PatternAST` as `TypeAST*`).
3. **Change type aliases:**
   - Replace `std::unique_ptr` container aliases with `ArenaSpan` aliases where appropriate, or use `ArenaSpan` inside the node classes.
4. **Update `ProgramAST`:**
   - Change `decls` from `std::vector<DeclPtr>` to `ArenaSpan<DeclPtr>`.
   - Add `InternedString filePath;` to `ProgramAST` (which acts as the single source of truth for the file path).
5. **Update `AttributeAST`:**
   - Change `args` from `std::vector<ASTPtr<AttributeArgAST>>` to `ArenaSpan<ASTPtr<AttributeArgAST>>`.

#### [MODIFY] [DeclAST.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/DeclAST.hpp)
- Add `std::optional<DocComment> doc;` and `ArenaSpan<AttributePtr> attributes;` to `DeclAST` base struct.
- Convert all `std::vector` properties to `ArenaSpan`:
  - `ImportDeclAST::path`
  - `FuncDeclAST::genericParams`, `paramGroups`
  - `StructDeclAST::genericParams`, `fields`
  - `EnumDeclAST::variants`
  - `TraitDeclAST::genericParams`, `methods`
  - `ImplDeclAST::genericParams`, `methods`
  - `FromDeclAST::genericParams`, `entries`

#### [MODIFY] [ExprAST.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/ExprAST.hpp)
- Add `TypeAST* resolvedType = nullptr;` to `ExprAST` base struct.
- Convert all `std::vector` properties to `ArenaSpan`:
  - `ArrayLiteralExprAST::elements`
  - `StructLiteralExprAST::genericArgs`, `inits`
  - `BehaviorAccessExprAST::concreteTypeArgs`, `genericArgs`
  - `CallExprAST::genericArgs`, `args`
  - `NullableChainExprAST::steps`
  - `PipelineStepAST::genericArgs`, `packArgs`
  - `PipelineExprAST::steps`
  - `ComposeOperandAST` references
  - `ComposeExprAST::operands`
  - `MatchArmAST::patterns`, `exprs`
  - `MatchExprAST::arms`
  - `StructPatternAST::fields`

#### [MODIFY] [StmtAST.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/StmtAST.hpp)
- Convert all `std::vector` properties to `ArenaSpan`:
  - `BlockStmtAST::stmts`
  - `SwitchCaseAST::values`
  - `SwitchStmtAST::cases`
  - `ReturnStmtAST::values`
  - `MultiVarDeclAST::vars`
  - `MultiAssignStmtAST::lhs`

#### [MODIFY] [TypeAST.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/TypeAST.hpp)
- Convert all `std::vector` properties to `ArenaSpan`:
  - `NamedTypeAST::genericArgs`
  - `FuncSignature::paramGroups`, `returnTypes`, `rawQualifiers`

---

## Verification Plan

### Manual Verification
1. We will verify that the compilation of files under `src/ast/` succeeds when compiling them individually (or header checks).
2. We will analyze the sizes of `sizeof(BaseAST)` and `sizeof(ExprAST)` before/after using a diagnostic scratch test if possible to ensure we successfully shrunk the memory footprints.
