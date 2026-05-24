# AST Nodes for `!` Error Handling — Finalized Plan

## Background

The grammar was updated with a full error-handling system centred on `!`. The `!` operator appears in **three distinct syntactic contexts**, each with a different semantic role. This plan documents what is already covered by existing AST nodes, what new nodes are needed, and what stale code to remove.

---

## Summary of `!` Usages in the Grammar

### 1. `!` as a type suffix — `result_suffix`

```
result_suffix   := '!' type     -- success T, failure E
                 | '!'          -- success T, failure nil
```

Examples: `int!string`, `int!`, `int?!string`, `int?!`

**Status: MISSING → add `ResultTypeAST`**

---

### 2. `!` as an argument pack annotation — pipeline step

```
pipeline_step   := IDENTIFIER '(' arg_list ')' '!'
postfix_op      := '(' [ arg_list ] ')' '!'
```

Example: `v |> scale(2.0)!`

**Status: ✅ COVERED** — `PipelineStepKind::ArgPack` (and variants) + `CallExprAST::isArgPack` already exist.

---

### 3. `resolve` expression — structured error unwrapping

```
resolve_expr    := 'resolve' expr '{' ok_arm err_arm '}'
ok_arm          := 'ok'  '(' IDENTIFIER type ')' block
err_arm         := 'err' '(' [ IDENTIFIER type ] ')' block
```

Example: `resolve f() { ok (v int) { return v } err (e string) { return -1 } }`

**Status: MISSING → add `ResolveExprAST`, `OkArmAST`, `ErrArmAST`**

---

### 4. `??` fallback on `T!E`

**Status: ✅ COVERED** — `NullCoalesceExprAST` already handles both nil and unresolved `!` fallback uniformly.

---

## Gap & Cleanup Summary

| Item | Action | File |
|---|---|---|
| `T!E` / `T!` result type | **Add** `ResultTypeAST` | `TypeAST.hpp` |
| `resolve` expression | **Add** `ResolveExprAST` | `ExprAST.hpp` |
| `ok` arm | **Add** `OkArmAST` | `ExprAST.hpp` |
| `err` arm | **Add** `ErrArmAST` | `ExprAST.hpp` |
| `StaticAccessExprAST` | **Remove** forward decl, visitor overload | `BaseAST.hpp` |
| Argument pack `fn(args)!` | No change needed | — |
| `??` fallback | No change needed | — |

---

## Proposed Changes

---

### Component 1 — Remove `StaticAccessExprAST`

#### [MODIFY] [BaseAST.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/BaseAST.hpp)

**Remove** the forward declaration on line 84:
```diff
- struct StaticAccessExprAST;
```

**Remove** the visitor overload from `ASTVisitor`:
```diff
- virtual void visit(StaticAccessExprAST&)    {}
```

> [!NOTE]
> No `ASTKind::StaticAccess` exists, so the enum needs no change. Check `ExprAST.hpp` to confirm no partial definition was accidentally left in — the grep shows it's forward-decl-only.

---

### Component 2 — `ResultTypeAST` in TypeAST.hpp

#### [MODIFY] [TypeAST.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/TypeAST.hpp)

Add `ResultTypeAST` after `NullableTypeAST` (around line 181):

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// ResultTypeAST – the `!` suffix: success type T paired with error type E.
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Wraps a success type with an optional error type using the `!` suffix.
 *
 * @example
 *   int!string   → inner = PrimitiveTypeAST(Int),  errorType = PrimitiveTypeAST(String)
 *   int!         → inner = PrimitiveTypeAST(Int),  errorType = nullptr  (bare '!')
 *   int?!string  → inner = NullableTypeAST(Int),   errorType = PrimitiveTypeAST(String)
 *   int?!        → inner = NullableTypeAST(Int),   errorType = nullptr
 *
 * Grammar rules enforced by the semantic pass:
 *   - Neither `inner` nor `errorType` may itself be a ResultTypeAST
 *     (nesting '!' is forbidden — see §Nesting `!` is Forbidden in grammar)
 *   - `?` always comes before `!` when both are present (inner is NullableTypeAST)
 *   - `!` is NEVER valid directly after an array type or inline function type —
 *     use a named alias first (same rule as `?`)
 */
struct ResultTypeAST : TypeAST {
    static constexpr ASTKind staticKind = ASTKind::ResultType;

    TypePtr inner;       ///< The success type (T in T!E or T?!E)
    TypePtr errorType;   ///< The error type E; nullptr means bare '!' (fails with nil)

    ResultTypeAST(TypePtr t, TypePtr err)
        : TypeAST(ASTKind::ResultType),
          inner(std::move(t)), errorType(std::move(err)) {}

    /// Convenience: true when this is a bare '!' with no error payload
    bool hasErrorType() const { return errorType != nullptr; }

    void accept(ASTVisitor& v) override { v.visit(*this); }
};
```

Also update the `@grammar` block at the top of the file to reflect `result_suffix`.

---

### Component 3 — `OkArmAST`, `ErrArmAST`, `ResolveExprAST` in ExprAST.hpp

#### [MODIFY] [ExprAST.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/ExprAST.hpp)

Add the three new nodes after `AwaitExprAST` (around line 676), before the control-flow expression section:

```cpp
// ═════════════════════════════════════════════════════════════════════════════
// RESOLVE NODES — structured error unwrapping
// ═════════════════════════════════════════════════════════════════════════════

/**
 * @brief The `ok` arm of a resolve expression.
 *
 * @example
 *   ok (v int)    { return v }
 *   ok (v int?)   { return v ?? 0 }
 *
 * Grammar: 'ok' '(' IDENTIFIER type ')' block
 *
 * `bindType` is always plain T — never T!E. The `!` is consumed at the
 * resolve boundary; the ok arm receives the already-unwrapped success value.
 *
 * Extends BaseAST (not StmtAST) — mirrors MatchArmAST / DefaultArmAST.
 */
struct OkArmAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::OkArm;

    InternedString bindName;   ///< Name of the success variable (e.g. "v")
    TypePtr        bindType;   ///< Plain T (never T!E — ! is consumed by resolve)
    StmtPtr        body;       ///< Always BlockStmtAST

    OkArmAST() : BaseAST(ASTKind::OkArm) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using OkArmPtr = ASTPtr<OkArmAST>;

/**
 * @brief The `err` arm of a resolve expression.
 *
 * @example
 *   err (e string) { return -1 }    -- typed error: E = string
 *   err ()         { return 0  }    -- bare '!': no error payload
 *
 * Grammar: 'err' '(' [ IDENTIFIER type ] ')' block
 *
 * When the result type used bare `!` (no error type), the parens are empty:
 *   `bindName` is empty string, `bindType` is nullptr.
 *
 * Extends BaseAST (not StmtAST) — mirrors MatchArmAST / DefaultArmAST.
 */
struct ErrArmAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::ErrArm;

    InternedString bindName;   ///< Error variable name; empty string when bare '!'
    TypePtr        bindType;   ///< Error type E; nullptr when bare '!' (no error value)
    StmtPtr        body;       ///< Always BlockStmtAST

    /// True when the enclosing result type was bare '!' (no error payload)
    bool isBareError() const { return bindType == nullptr; }

    ErrArmAST() : BaseAST(ASTKind::ErrArm) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};

using ErrArmPtr = ASTPtr<ErrArmAST>;

/**
 * @brief Structured resolution of a T!E value — forces handling of both outcomes.
 *
 * @example
 *   resolve divide(10, 0) {
 *       ok  (v int)    { return v  }
 *       err (e string) { return -1 }
 *   }
 *
 * Grammar: 'resolve' expr '{' ok_arm err_arm '}'
 *
 * The `subject` must resolve to a T!E type. After the resolve block, the `!`
 * is consumed and the result is plain T (the type returned by the ok arm).
 * Both arms are required; both must return the same type.
 *
 * Listed in `primary_expr` in the grammar — this is an expression, not a
 * statement, exactly like MatchExprAST.
 */
struct ResolveExprAST : ExprAST {
    static constexpr ASTKind staticKind = ASTKind::ResolveExpr;

    ExprPtr    subject;   ///< The T!E expression being resolved
    OkArmPtr   okArm;     ///< Required ok arm
    ErrArmPtr  errArm;    ///< Required err arm

    ResolveExprAST() : ExprAST(ASTKind::ResolveExpr) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};
```

---

### Component 4 — `ASTKind`, forward declarations, and `ASTVisitor` in BaseAST.hpp

#### [MODIFY] [BaseAST.hpp](file:///c:/Users/TaiAx/Desktop/luc/src/ast/BaseAST.hpp)

**Forward declarations — add:**
```cpp
// TypeAST.hpp
struct ResultTypeAST;   // NEW

// ExprAST.hpp
struct ResolveExprAST;  // NEW
struct OkArmAST;        // NEW
struct ErrArmAST;       // NEW
```

**Forward declarations — remove:**
```diff
- struct StaticAccessExprAST;
```

**`ASTKind` enum — add to type nodes section:**
```diff
  // Type nodes
  PrimitiveType,
  NamedType,
  NullableType,
+ ResultType,       // NEW — T!E / T!
  FixedArrayType,
  ...
```

**`ASTKind` enum — add to expression nodes section:**
```diff
  // Expression nodes
  ...
  AwaitExpr,
+ ResolveExpr,      // NEW — resolve expr { ok ... err ... }
+ OkArm,            // NEW — ok (v T) { ... }
+ ErrArm,           // NEW — err (e E) { ... }
  MatchExpr,
  ...
```

**`ASTVisitor` — add:**
```cpp
// Type nodes
virtual void visit(ResultTypeAST&)     {}   // NEW

// Expression nodes
virtual void visit(ResolveExprAST&)    {}   // NEW
virtual void visit(OkArmAST&)          {}   // NEW
virtual void visit(ErrArmAST&)         {}   // NEW
```

**`ASTVisitor` — remove:**
```diff
- virtual void visit(StaticAccessExprAST&)    {}
```

---

## Design Rationale

### `OkArmAST` / `ErrArmAST` extend `BaseAST` (not `StmtAST`)
Confirmed. Mirrors `MatchArmAST` and `DefaultArmAST` — arm nodes are structural containers owned by their parent expression, not independently executable statements.

### `ResultTypeAST` keeps `inner` + `errorType` separate
The nesting prohibition (`T!E` where T or E themselves are `T!E`) is a **semantic** constraint, not a parser constraint. The parser builds `ResultTypeAST` freely; the semantic pass walks both `inner` and `errorType` and emits an error if either is `ASTKind::ResultType`. This keeps the parser simple and puts the rule where it belongs — in `TypeResolver`.

### `??` unchanged
`NullCoalesceExprAST` already handles both `nil` fallback and `T!E` fallback uniformly per the grammar spec. No structural change needed — the semantic pass will annotate which case applies.

### `StaticAccessExprAST` removed
Leftover from old code. Forward declaration in `BaseAST.hpp` and visitor stub removed. No `ASTKind` or definition exists, so removal is contained to those two sites.

---

## Verification Plan

### Build Check
- `cmake --build` must succeed with zero errors and zero new warnings after changes.
- Grep for `StaticAccessExprAST` across `src/` — must return zero results after removal.

### Node Completeness Check
- Every new `ASTKind` entry must appear in: the enum, a forward declaration, the visitor, and the node definition.
- `ResultTypeAST` must appear in `TypeAST.hpp` and be included wherever type nodes are consumed.

### Semantic Notes (for later — not part of this AST-only change)
- `TypeResolver` must reject `ResultTypeAST` where `inner` or `errorType` is itself `ASTKind::ResultType`.
- `TypeResolver` must reject `ResultTypeAST` where `inner` is `ASTKind::FuncType` or any array type without an alias.
