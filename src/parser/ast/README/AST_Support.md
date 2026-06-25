# AST Support Library

## Overview

This directory contains foundational utilities that enable the Luc compiler's AST to be **memory-efficient**, **fast**, and **arena-allocated**. These components work together to eliminate per-node heap allocations, reduce memory footprint by 8x for identifiers, and provide safe read-only views into contiguous memory.

## Quick Reference

| File                 | Purpose                        | When to Use                             |
| -------------------- | ------------------------------ | --------------------------------------- |
| `InternedString.hpp` | 32-bit handle for strings      | Every identifier, type name, field name |
| `StringPool.hpp/cpp` | Owns interned string data      | One per compilation session             |
| `ArenaSpan.hpp`      | Read-only view of arena arrays | All child lists in AST nodes            |
| `ASTArena.hpp`       | Bump allocator for AST nodes   | All AST node allocations                |

## File-by-File Breakdown

### 1. `InternedString.hpp`

**What it does:** Replaces `std::string` with a 4-byte integer ID.

```cpp
// Before: 32+ bytes per string reference
struct VarDeclAST {
    std::string name;  // 32+ bytes
};

// After: 4 bytes per string reference
struct VarDeclAST {
    InternedString name;  // 4 bytes
};
```

**Why it exists:**
- `std::string` is 32+ bytes on 64-bit systems (pointer, size, capacity, SSO buffer)
- Thousands of AST nodes storing the same "Vec2" means thousands of copies
- String comparisons become `uint32_t` equality (1 CPU instruction vs O(n))

**Performance impact:**
- Memory: 8x reduction for name fields
- Speed: O(1) comparison vs O(n) string comparison
- Hashing: trivial (just hash the 32-bit ID)

**If removed:** You'd need to store `std::string` everywhere → 8x memory increase, slower symbol resolution, more heap allocations.

---

### 2. `StringPool.hpp` / `StringPool.cpp`

**What it does:** Owns the actual string data and maps IDs ↔ strings.

```cpp
StringPool pool;
InternedString id = pool.intern("Vec2");     // Returns ID 1
std::string_view str = pool.lookup(id);     // Returns "Vec2"
pool.intern("Vec2");                        // Returns same ID 1 (deduplicated)
```

**Why it exists:**
- Centralizes string ownership (single source of truth)
- Deduplicates identical strings across the entire compilation
- Provides stable `std::string_view` pointers that never invalidate
- ID 0 reserved for empty/invalid string (no allocation)

**Design decisions:**
- **Bump allocator** for string characters (64 KB blocks) → no per-string malloc
- **ID 0 convention** → `InternedString()` is already valid "empty" string
- **Session-scoped** → one pool per compiler session, shared across files

**If removed:** You'd need per-string heap allocation, manual deduplication, and lose cross-file ID consistency.

---

### 3. `ArenaSpan.hpp`

**What it does:** A non-owning, read-only view into contiguous arena memory.

```cpp
// Building a list of child nodes
auto builder = arena.makeBuilder<ExprPtr>();
builder.push_back(expr1);
builder.push_back(expr2);
ArenaSpan<ExprPtr> children = builder.build();

// Usage
for (const auto& child : children) {
    // read-only access
}
```

**Why it exists:**
- `std::vector` allocates its own heap buffer → breaks arena contiguity
- `std::vector` has destructor that iterates over elements → unnecessary for arena
- ArenaSpan is just a `{pointer, size}` pair (16 bytes) with no destructor

**Comparison:**

| Feature         | `std::vector<T>`             | `ArenaSpan<T>`      |
| --------------- | ---------------------------- | ------------------- |
| Owns memory     | Yes                          | No (views arena)    |
| Heap allocation | Yes (per vector)             | No                  |
| Destructor      | Calls ~T on elements         | None (trivial)      |
| Size            | 24 bytes (ptr+size+capacity) | 16 bytes (ptr+size) |
| Mutability      | Mutable                      | Read-only           |

**If removed:** You'd use `std::vector` → each AST node's child list becomes a separate heap allocation (fragmentation, slower cache locality).

---

### 4. `ASTArena.hpp`

**What it does:** Bump allocator that allocates all AST nodes in large contiguous blocks.

```cpp
ASTArena arena;
auto* var = arena.make<VarDeclAST>(name, type, init);
auto* block = arena.make<BlockStmtAST>();
auto* exprs = arena.allocArray<ExprPtr>({expr1, expr2});
```

**Why it exists:**
- **No per-node malloc** → single allocation per block (64 KB default)
- **No destructor calls** → arena is destroyed as a whole at end of compilation
- **Better cache locality** → all AST nodes are contiguous in memory
- **Simplified memory management** → no need to track individual node lifetimes

**How it works:**
1. Allocates 64 KB blocks from the system
2. Bumps a pointer forward for each allocation
3. When block is exhausted, allocates a new block
4. At destruction, all blocks are freed at once

**Performance impact:**
- Allocation speed: ~10x faster than `new`
- Memory locality: AST traversal uses CPU cache efficiently
- No fragmentation: allocations are packed tightly

**If removed:** You'd use `std::unique_ptr` or raw `new`/`delete` → slower allocation, memory fragmentation, need to manage destructors.

---

## How They Work Together

```
┌─────────────────────────────────────────────────────────────────┐
│                         CompilerSession                         │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────────────┐  │
│  │ StringPool  │    │  ASTArena   │    │   TypeResolver      │  │
│  │             │    │             │    │                     │  │
│  │ - "Vec2"    │◄───│ - VarDecl   │    │ - typesEqual()      │  │
│  │ - "int"     │    │ - FuncDecl  │    │ - unwrapAlias()     │  │
│  │ - "x"       │    │ - BlockStmt │    │                     │  │
│  └─────────────┘    └─────────────┘    └─────────────────────┘  │
│         │                  │                    │               │
│         ▼                  ▼                    ▼               │
│  InternedString(1)    ArenaSpan<ExprPtr>    Type comparison     │
│  is 4 bytes           is 16 bytes           uses integer IDs    │
└─────────────────────────────────────────────────────────────────┘
```

**Data flow example:**

```cpp
// 1. Parser interns string → gets 4-byte ID
InternedString typeName = pool.intern("Vec2");

// 2. AST node stores 4-byte ID (not 32+ byte string)
auto* var = arena.make<VarDeclAST>(typeName, ...);

// 3. Child lists use ArenaSpan (no separate heap allocation)
auto* structDecl = arena.make<StructDeclAST>();
auto fields = arena.makeBuilder<FieldDeclPtr>();
fields.push_back(field1);
fields.push_back(field2);
structDecl->fields = fields.build();  // ArenaSpan<FieldDeclPtr>

// 4. Type resolution uses integer comparison (fast!)
if (typeName == otherTypeName) { ... }
```

---

## Technical Decisions Summary

### Pros (Keep these files)

| Aspect            | Benefit                                                      |
| ----------------- | ------------------------------------------------------------ |
| **Memory**        | 8x reduction for string fields, zero per-node heap overhead  |
| **Speed**         | O(1) string comparison, ~10x faster allocation               |
| **Cache**         | Contiguous AST layout → better traversal performance         |
| **Simplicity**    | No manual memory management, no destructors to write         |
| **Deduplication** | Strings automatically deduplicated across entire compilation |

### Cons (Challenges to be aware of)

| Aspect                 | Risk                                                           | Mitigation                                         |
| ---------------------- | -------------------------------------------------------------- | -------------------------------------------------- |
| **String lifetime**    | `std::string_view` into pool becomes invalid if pool destroyed | Pool lives for entire compilation session          |
| **No mutation**        | `ArenaSpan` is read-only                                       | Build phase uses `SpanBuilder`, then freeze        |
| **Debugging**          | Can't see string values in debugger easily                     | Use `pool.lookup(id)` in watch window              |
| **Dependency**         | Functions need `StringPool&` parameter                         | Explicit dependency is good (no hidden state)      |
| **No partial cleanup** | Can't free individual nodes                                    | Not needed for compiler (entire AST freed at once) |

### When to Add More

Consider adding if you need:

| Feature                       | Would require                                          |
| ----------------------------- | ------------------------------------------------------ |
| **Small string optimization** | Modify `StringPool` to store small strings inline      |
| **Arena resizing**            | Add `reserve()` to `ASTArena`                          |
| **Memory tracking**           | Add debug counters to both allocators                  |
| **Parallel compilation**      | Add thread-local arenas, shared string pool with locks |

### When to Remove

**Don't remove these** unless you have a compelling reason:

| If removed       | Consequence                                           |
| ---------------- | ----------------------------------------------------- |
| `InternedString` | 8x memory increase, slower comparisons                |
| `StringPool`     | Can't deduplicate strings, per-string heap allocation |
| `ArenaSpan`      | Each child list becomes separate heap allocation      |
| `ASTArena`       | Per-node malloc, destructor management, fragmentation |

**Valid reasons to remove/replace:**
- Targeting a platform with severe memory constraints (embedded)
- Need precise control over individual node lifetimes
- Integrating with a garbage-collected runtime
- Simplifying for a teaching compiler (though these patterns are worth learning)

---

## Quick Decision Guide

### "Should I use ArenaSpan or std::vector?"

| Use `ArenaSpan<T>` when...      | Use `std::vector<T>` when...     |
| ------------------------------- | -------------------------------- |
| Storing AST child nodes         | Temporary storage during parsing |
| Memory is arena-allocated       | You need to modify after freeze  |
| Read-only access after build    | You need dynamic resizing        |
| Maximum performance is critical | The data outlives the arena      |

### "Should I use InternedString or std::string?"

| Use `InternedString` when...  | Use `std::string` when...      |
| ----------------------------- | ------------------------------ |
| Storing identifiers/names     | Generating diagnostic messages |
| Frequent equality comparisons | String manipulation needed     |
| AST node fields               | Temporary values               |
| Symbol table keys             | Data from external sources     |

### "Should I use ASTArena or new/delete?"

| Use `ASTArena` when...     | Use `new`/`delete` when...            |
| -------------------------- | ------------------------------------- |
| Allocating AST nodes       | Objects with complex lifetimes        |
| Performance is critical    | Need individual destructor calls      |
| Batch deallocation is fine | Objects outlive the arena             |
| Building an AST            | Building a symbol table that persists |

---

## Common Pitfalls

### ❌ Storing ArenaSpan across arena resets
```cpp
ArenaSpan<ExprPtr> saved;  // BAD: arena may be reset
{
    ASTArena temp;
    saved = temp.allocArray<ExprPtr>({expr});
}  // temp destroyed, saved now dangling
```

### ❌ Returning std::string_view from StringPool
```cpp
std::string_view getName(InternedString id) {
    return pool.lookup(id);  // OK if pool alive
}
// But storing the view long-term is dangerous
```

### ❌ Mixing arena and heap allocation
```cpp
auto* node = arena.make<VarDeclAST>(...);
node->extraData = new std::string(...);  // BAD: defeats arena benefits
```

### ✅ Correct patterns
```cpp
// Store InternedString, not std::string
struct FieldDeclAST {
    InternedString name;      // 4 bytes
    TypeAST* type;            // 8 bytes
};

// Build then freeze
auto builder = arena.makeBuilder<ExprPtr>();
builder.push_back(expr);
ArenaSpan<ExprPtr> frozen = builder.build();  // Now read-only

// Use pool for temporary strings
void reportError(InternedString name) {
    std::string_view nameStr = pool.lookup(name);  // Temporary view
    std::cout << "Error: " << nameStr;
}
```

---

## File Dependencies

```
InternedString.hpp (no deps)
       │
       ▼
StringPool.hpp ──► InternedString.hpp
       │
       ▼
ASTArena.hpp ──► ArenaSpan.hpp
       │
       ▼
ArenaSpan.hpp (no deps)
```

All files are **header-only except StringPool.cpp** (which has a small implementation).

---

## Final Summary

| File             | Role           | Memory Impact          | Speed Impact          | Remove? |
| ---------------- | -------------- | ---------------------- | --------------------- | ------- |
| `InternedString` | String handles | 8x reduction           | 10x faster comparison | ❌ No    |
| `StringPool`     | String storage | Deduplication          | O(1) lookup           | ❌ No    |
| `ArenaSpan`      | Array views    | No per-list allocation | Better locality       | ❌ No    |
| `ASTArena`       | Node allocator | No per-node malloc     | 10x faster alloc      | ❌ No    |

**These files form the foundation of the Luc compiler's memory strategy.** Removing them would require rewriting the AST layer and sacrificing significant performance gains. Keep them unless you have a very specific reason to replace them.
```

This README provides:
1. **Quick overview** - one glance at the table tells you what each file does
2. **Basic understanding** - explains structure, usage patterns, and dependencies
3. **Technical decision support** - pros/cons table, comparison with alternatives, when to keep/remove