## Implementation Plan: AST & Compiler Infrastructure Upgrades

This plan details how to evolve your existing AST foundation into a faster, safer, and more maintainable representation. The changes are ordered by **impact vs. effort**, respecting your current architecture (BaseAST hierarchy, visitor pattern, thin family bases).

---

### Phase 0 – Prerequisites & Guidelines
Before touching code, agree on these patterns to keep the codebase consistent:

- **All new fields on BaseAST** must be zero-initialized and never require manual cleanup (use value types or plain pointers into arena).
- **Parser is the only producer** of AST nodes; Semantic passes are consumers and annotators (they write to `resolvedType`, new flags, etc.).
- **Arena allocation** will be introduced behind the existing `std::unique_ptr` factories so that most code doesn’t change.
- **No family headers get new `#include` dependencies** – keep the include graph acyclic.

---

### Phase 1 – Quick Wins: Parent Pointer + Effect Bitset  
*Effort: ~3–4 hours | Risk: None*

**Goal:** Give every node knowledge of its parent and a lightweight summary of its runtime behaviour.

#### 1.1 Add fields to `BaseAST`
In `BaseAST.hpp`, inside `struct BaseAST`:

```cpp
// ── Structural
BaseAST* parent = nullptr;   // set by parser when child is attached

// ── Behavioural bitmask
uint32_t effectFlags = 0;
```

Add an enum above the class:

```cpp
enum class Effect : uint32_t {
    None          = 0,
    SideEffect    = 1 << 0,  // writes memory, calls impure function, I/O
    IsAsync       = 1 << 1,  // contains await or async call
    WritesMemory  = 1 << 2,  // direct assignment to memory location
    ReadsMemory   = 1 << 3,  // dereference or non-const field access
    Tainted       = 1 << 4,  // raw pointer (*T) involved, FFI boundary
};

inline constexpr Effect operator|(Effect a, Effect b) { return static_cast<Effect>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }
inline bool hasEffect(uint32_t flags, Effect e) { return (flags & static_cast<uint32_t>(e)) != 0; }
```

#### 1.2 Populate `parent` during parsing
Every time the parser constructs a node and attaches it as a child, set `child->parent = parentNode`. Example for a binary expression creation:

```cpp
auto bin = std::make_unique<BinaryExprAST>();
bin->left = parseExpr();   // inside parseExpr, after creating the child, do
// bin->left->parent = bin.get();  // do this right after attachment
```

Consider a small helper:

```cpp
template<typename T, typename U>
void adopt(T& parent, std::unique_ptr<U>& child) {
    if (child) child->parent = &parent;
}
```

#### 1.3 Populate effect flags
In the parser, set flags immediately for leaves:

- `LiteralExprAST`: only `ReadsMemory` if it’s a string or array literal? Actually leave `None`; effects propagate upwards.
- `AssignExprAST`: `SideEffect | WritesMemory`.
- `CallExprAST`: `SideEffect` (unless callee is known pure later).
- `AwaitExprAST`: `SideEffect | IsAsync`.
- Nodes referencing raw pointers (`*T`, `@ptrToRef`): `Tainted`.

These flags can be refined by the semantic pass later.

#### 1.4 Use flags in semantic checks
Now parallel body validation becomes:

```cpp
// inside visit(ParallelForStmtAST&)
if (hasEffect(bodyStmt->effectFlags, Effect::SideEffect | Effect::IsAsync))
    error(...);
```

Scope-depth checks (e.g., `break` outside loop) can walk `parent` instead of maintaining an explicit stack in the visitor.

---

### Phase 2 – Identifier Interning  
*Effort: ~1–2 days | Risk: Low – careful migration required*

**Goal:** Replace all `std::string` identifiers (names, field names, type names) with a light-weight `InternedString` to reduce memory, speed up comparisons, and simplify hashing.

#### 2.1 Create a `StringPool` utility
In `src/support/StringPool.hpp`:

```cpp
class StringPool {
    std::vector<std::unique_ptr<char[]>> blocks;
    // ... bump allocation of char sequences, deduplication via hash map
public:
    uint32_t intern(const std::string& s);
    const std::string& lookup(uint32_t id) const;
    static StringPool& instance();  // global singleton for simplicity
};
```

#### 2.2 Introduce `InternedString` wrapper
In `BaseAST.hpp` (or a separate header that BaseAST.hpp includes, like `debug/DebugMacros.hpp` might be feasible, but careful with cycles; better place in a new `support/InternedString.hpp` that has no dependency on AST):

```cpp
struct InternedString {
    uint32_t id = 0; // 0 = invalid
    InternedString() = default;
    explicit InternedString(uint32_t id) : id(id) {}
    bool operator==(InternedString other) const { return id == other.id; }
    // for debugging:
    const std::string& str() const { return StringPool::instance().lookup(id); }
};
```

#### 2.3 Change all AST name fields
Replace `std::string name;` in every node (ParamAST, VarDeclAST, FuncDeclAST, FieldInitAST, etc.) with `InternedString name;`. The constructor signature will change from `std::string n` to `InternedString n`, but the parser will pass `InternedString(pool.intern("foo"))`.

Be systematic: search for `std::string name` in all AST headers and replace. Also handle `std::string typeName` in `BehaviorAccessExprAST`, `StructLiteralExprAST`, etc., and `std::string method`, `field`, `ident`, etc.

#### 2.4 Update visitors and semantic pass
Wherever code used `node->name == "something"`, it now becomes `node->name == InternedString(pool.intern("something"))`. To make this less verbose, provide a helper: `bool is(InternedString s, const char* literal) { return s.id == StringPool::instance().intern(literal).id; }`. But you can also just compare IDs after obtaining the literal’s ID once.

#### 2.5 Update parser
The lexer already produces `std::string` tokens. The parser will immediately intern each identifier token value and pass the `InternedString` to node constructors. Wrap token `std::string` into interned ID.

**Benefits:** Memory savings (~40% of identifier strings), `O(1)` comparisons, faster hash map lookups in symbol tables.

---

### Phase 3 – Arena Allocator  
*Effort: ~3–5 days | Risk: Medium – affects node ownership*

**Goal:** Allocate all AST nodes contiguously for cache-friendly traversal, and deallocate the whole tree in one shot.

#### 3.1 Implement `ASTArena`
```cpp
class ASTArena {
    std::vector<std::unique_ptr<char[]>> blocks;
    char* cur = nullptr;
    size_t left = 0;
    static constexpr size_t BlockSize = 64 * 1024;
public:
    template<typename T, typename... Args>
    T* alloc(Args&&... args) {
        constexpr size_t size = sizeof(T);
        constexpr size_t align = alignof(T);
        // align cur, allocate, placement new
        return new(ptr) T(std::forward<Args>(args)...);
    }
};
```
Provide a global (or per‑translation‑unit) arena instance.

#### 3.2 Change ownership to arena-aware pointers
Keep `using ExprPtr = std::unique_ptr<ExprAST, ASTDeleter>;` where `ASTDeleter` is a no‑op (arena will free block). For safety, you can define:

```cpp
struct ASTDeleter {
    void operator()(BaseAST*) const { /* no-op */ }
};
```
Now the ownership aliases become:

```cpp
using TypePtr    = std::unique_ptr<TypeAST, ASTDeleter>;
using DeclPtr    = std::unique_ptr<DeclAST, ASTDeleter>;
using ExprPtr    = std::unique_ptr<ExprAST, ASTDeleter>;
using StmtPtr    = std::unique_ptr<StmtAST, ASTDeleter>;
using PatternPtr = std::unique_ptr<PatternAST, ASTDeleter>;
```

#### 3.3 Modify parser node creation
Instead of `std::make_unique<T>(...)`, use a factory that allocates via arena and wraps in the custom deleter:

```cpp
template<typename T, typename... Args>
ExprPtr makeExpr(Args&&... args) {
    T* ptr = currentArena.alloc<T>(std::forward<Args>(args)...);
    return ExprPtr(ptr, ASTDeleter{});
}
```
Replace all `std::make_unique<T>(...)` calls in parser with `makeExpr<T>(...)` etc.

#### 3.4 Implication for mutable fields
Fields like `StructDeclAST::selfType` (a `unique_ptr<NamedTypeAST>`) are no longer heap‑allocated via `new`; they must also be arena‑allocated. Since `selfType` is lazily created and the arena never deallocates until end‑of‑compilation, that’s fine. The pointer remains stable.

#### 3.5 Lifetime management
The arena (one per translation unit) lives for the duration of compiling that file. After codegen, the arena is destroyed, freeing all AST memory at once. Make sure no node destructors rely on `delete` of unique_ptr members (they don’t, since all members are unique_ptrs with no-op deleters).

---

### Phase 4 – Constant Folding Pass  
*Effort: ~1 day | Risk: Low – new visitor, no AST changes*

**Goal:** Replace compile‑time‑constant subtrees with a single literal node.

#### 4.1 Write `ConstantFoldingVisitor`
Inherit from `ASTVisitor`, override `visit(BinaryExprAST&)`, `visit(UnaryExprAST&)`, etc. For example, if both children of a `+` node are literal ints, create a new `LiteralExprAST` and return it (or replace in place via parent pointer). This pass should run after type‑checking (so types are known). It can be run as a separate step in the semantic pipeline.

#### 4.2 Replacement strategy
For each eligible node, create a literal node (arena alloc), copy source location, and then replace the node in parent’s child vector. Since you have `parent` pointers, you can climb up and swap.

#### 4.3 Integration
After semantic analysis, call `ConstantFoldingVisitor::run(program)` before codegen. This reduces IR complexity and catches some errors (e.g., array sizes) at compile time.

---

### Phase 5 – Taint Tracking & Unsafe Isolation  
*Effort: ~2 days | Risk: Medium – changes semantic rules*

#### 5.1 Extend effect flags
Already added `Tainted` flag. In the parser, mark:

- `PtrTypeAST` nodes as `Tainted` (when used as a type).
- Any expression containing a raw pointer operation (`@ptrToRef`, `@ptrOffset`, `*` cast) as `Tainted`.

#### 5.2 Semantic rules
During type‑checking, when assigning a tainted expression to a safe (non‑tainted) variable/field, enforce that an explicit conversion intrinsic (`@ptrToRef`) is present. The visitor will check the `effectFlags` of the rhs; if tainted && target type is not `*T`, error.

#### 5.3 Propagation
Taint propagates: any expression that contains a tainted sub‑expression is itself tainted (factored into effect flags via simple bitwise OR while traversing, already done in semantic pass).

---

### Phase 6 – Optional: Canonicalization / Lowering Pass  
*Effort: ~2–4 weeks | Risk: High – large refactor, postpone until needed*

This phase converts high‑level AST nodes (IfExprAST, ForStmtAST, etc.) into a simpler, linear IR closer to LLVM IR. It should be done after all semantic checks. This is a significant undertaking and only recommended when the compiler backend needs to support multiple targets or optimizations. For now, your current codegen can work directly with the rich AST.

---

## Implementation Sequence & Dependencies

1. **Phase 1 (Parent + Effects)** — no dependencies; can start immediately.
2. **Phase 2 (Interning)** — can overlap with Phase 1, but requires modifying many strings; do after API is stable.
3. **Phase 3 (Arena)** — best after interning, as node sizes may change slightly; can be done in parallel.
4. **Phase 4 (Constant Folding)** — requires Phase 1 (parent) for replacements; can be done after semantic analysis is mature.
5. **Phase 5 (Taint)** — builds on effects flags from Phase 1; add after core semantics are stable.
6. **Phase 6** — optional, much later.

Start with **Phase 1** today—you’ll feel the improvement immediately in your semantic passes. Then interning will clean up memory and speed up symbol table operations. With these in place, the arena will deliver the cache performance gain Gemini highlighted.