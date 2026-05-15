## Bugs and Inconsistencies ❌

### Bug 1 — `FuncBodyKind` still exists but is now a contradiction

`FuncBodyKind` is still defined and still stored on `FuncDeclAST`, `MethodDeclAST`, and `FromEntryAST`:

```cpp
// DeclAST.hpp line 141-145
enum class FuncBodyKind {
    Block,
    AnonFunc,
    ExprBody,
};
```

But the comment directly above it says:
> *"ExprBody is also desugared into a BlockStmtAST containing a ReturnStmtAST"*

And every node that uses it only has a single `StmtPtr body` (the dual `exprBody` was removed). If desugaring to `BlockStmtAST` always happens in the parser, then `FuncBodyKind::ExprBody` and `FuncBodyKind::AnonFunc` can never be observed by the semantic pass or codegen — the body is always a `BlockStmtAST` regardless. The field is dead weight that will confuse future readers.

**Fix:** Remove `FuncBodyKind` entirely. The body is always a `BlockStmtAST`; no kind tag is needed. If the original source form matters for error messages, a `bool isCurried` or similar narrow flag is cleaner.

---

### Bug 2 — `IfExprAST` comment says `StmtPtr` but fields are `ExprPtr`

The comment on `IfExprAST` (lines 884–886) still says:

```
// Both then and elseBranch are StmtPtr (BlockStmtAST) — the same block type
// used everywhere. The semantic pass checks that both branches produce the
// same type and that elseBranch is present.
```

But the actual fields are:

```cpp
ExprPtr  thenBranch;    // expression
ExprPtr  elseBranch;    // expression
```

The grammar `if_expr := 'if' expr '??' expr 'else' expr` confirms that both branches are **expressions**, not blocks — so `ExprPtr` is correct. The comment is a copy-paste leftover from the old design and is simply wrong. It needs to be corrected before someone changes the fields to match the misleading comment.

**Fix:** Update the comment to accurately say both branches are `ExprPtr` (plain expressions, not blocks).

---

### Bug 3 — `FieldPatternAST` comment still says `BaseAST*` but the field is now `unique_ptr<PatternAST>`

The comment at lines 1152–1154 says:

```
// subPattern is BaseAST* for the same reason as MatchArmAST::patterns —
// literal sub-patterns are LiteralExprAST (ExprAST), while structural
// sub-patterns are PatternAST; no common subtype tighter than BaseAST exists.
```

But the actual field is now:

```cpp
std::unique_ptr<PatternAST>  subPattern;
```

This is actually correct — `PatternExprAST` wraps literal/range nodes as `PatternAST`, so `PatternAST` is now the correct common base. But the comment still justifies the old `BaseAST*` design. It's misleading documentation.

**Fix:** Update the comment to explain that `PatternExprAST` bridges the gap so `PatternAST` is now the correct common type.

---

### Bug 4 — `MatchArmAST` comment still describes `vector<unique_ptr<BaseAST>>`

The comment block starting at line 1219 says:

```
// patterns — one or more patterns, comma-separated in source. Stored as
//   vector<unique_ptr<BaseAST>> rather than vector<unique_ptr<PatternAST>>
//   because literal and range arms now place LiteralExprAST and RangeExprAST
//   nodes here directly...
//   Valid ASTKind values in this vector:
//     LiteralExpr     — literal value pattern: 42, "ok", true
//     RangeExpr       — range pattern: 1..10, 1..<10
```

But the actual field is now:

```cpp
std::vector<std::unique_ptr<PatternAST>> patterns;
```

`LiteralExpr` and `RangeExpr` kinds are no longer valid directly — they are now wrapped in `PatternExprAST`. The comment lists the wrong type and wrong valid kinds.

**Fix:** Update the comment to list the correct valid `ASTKind` values: `BindPattern`, `WildcardPattern`, `TypePattern`, `StructPattern`, `PatternExpr` (which wraps `LiteralExpr` or `RangeExpr`).

---

### Bug 5 — `ExprAST.hpp` file-level comment (lines 13–17) contradicts the new design

The `@note` block at the top of `ExprAST.hpp` says:

```
// @note
//   - PatternAST is removed then merge into ExprAST(to reduce cyclic dependency).
//   - LiteralPatternAST and RangePatternAST are removed.
//   - LiteralExprAST and RangeExprAST are used directly in pattern position.
//   - MatchArmAST::patterns now holds BaseAST* instead of PatternAST*.
//   - StructPatternAST::subPattern now holds BaseAST* instead of PatternAST*.
```

Three of these five bullets are now wrong:
- `PatternAST` was **not** merged into `ExprAST` — it still exists as a separate `BaseAST` subclass with `BindPatternAST`, `WildcardPatternAST`, `TypePatternAST`, `StructPatternAST`, and `PatternExprAST` inheriting from it.
- `MatchArmAST::patterns` now holds `unique_ptr<PatternAST>`, not `BaseAST*`.
- `StructPatternAST::subPattern` now holds `unique_ptr<PatternAST>`, not `BaseAST*`.

**Fix:** Rewrite this `@note` block to accurately reflect the current design.

---

### Bug 6 — `PipelineStepAST::anonFunc` is `ExprPtr` but the comment says "forward declared below"

```cpp
// AnonFunc — inline anonymous function
ExprPtr  anonFunc;  // forward declared below
```

`AnonFuncExprAST` is defined **before** `PipelineStepAST` in `ExprAST.hpp` (it's in the FUNCTION NODES section, pipeline steps come after). The "forward declared below" comment is stale and inaccurate — `AnonFuncExprAST` is fully defined by the time `PipelineStepAST` is reached.

**Fix:** Remove the stale "forward declared below" comment. Just say `ExprPtr anonFunc; // AnonFuncExprAST`.

---

## Missing Things 🔴

### Missing 1 — `ForStmtAST` and `ParallelForStmtAST` still use `std::string varName` instead of a `ParamAST`

```cpp
struct ForStmtAST : StmtAST {
    std::string varName;    // "item", "i", "v"
    TypePtr     varType;    // optional explicit type (nullptr if inferred)
    ...
};
```

The for-loop iteration variable is a named, typed binding. Now that `ParamAST` is a proper AST node, `varName` + `varType` should be a single `ParamPtr iterVar` — it's the same concept as a function parameter: a name with an optional type annotation and a source location.

**Fix:**
```cpp
struct ForStmtAST : StmtAST {
    ParamPtr    iterVar;    // name + optional type — nullptr type = inferred
    ExprPtr     iterable;
    ExprPtr     step;
    StmtPtr     body;
};
```

Same fix for `ParallelForStmtAST`.

---

### Missing 2 — `SwitchCaseAST` is not a `BaseAST` node and has no `ASTKind`

```cpp
struct SwitchCaseAST {
    std::vector<ExprPtr>          values;
    std::unique_ptr<BlockStmtAST> body;
    SourceLocation                loc;
};
```

`SwitchCaseAST` is a plain struct with no `accept()`, no `kind`, and no visitor entry. The visitor cannot walk into switch cases uniformly. The comment says "No visitor dispatch is needed; the semantic pass reads it directly through the parent node" — but this is the same justification that was used for `PipelineStepAST` before it was promoted, and the same problems apply: the ASTDumper, semantic pass, and any future pass must all special-case it.

**Fix:** Promote it to a `BaseAST` node:
```cpp
// Add to ASTKind:
SwitchCase,

struct SwitchCaseAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::SwitchCase;
    std::vector<ExprPtr>          values;
    std::unique_ptr<BlockStmtAST> body;
    SwitchCaseAST() : BaseAST(ASTKind::SwitchCase) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};
```
Add `virtual void visit(SwitchCaseAST&) {}` to `ASTVisitor`.

---

### Missing 3 — `TraitRefAST` is not a `BaseAST` node

```cpp
struct TraitRefAST {
    std::string name;
    std::vector<TypePtr> genericArgs;
    SourceLocation loc;
};
```

Same problem as `SwitchCaseAST`. It has a location but no kind, no visitor, and no `accept()`. It appears inside `ImplDeclAST` and needs to be walked by the semantic pass when resolving trait conformance.

**Fix:**
```cpp
// Add to ASTKind:
TraitRef,

struct TraitRefAST : BaseAST {
    static constexpr ASTKind staticKind = ASTKind::TraitRef;
    std::string          name;
    std::vector<TypePtr> genericArgs;
    TraitRefAST() : BaseAST(ASTKind::TraitRef) {}
    void accept(ASTVisitor& v) override { v.visit(*this); }
};
```
Add `virtual void visit(TraitRefAST&) {}` to `ASTVisitor`. Add forward declaration to `BaseAST.hpp`.

---

### Missing 4 — `AttributeArgAST` is a plain struct with no location-aware visitor

```cpp
struct AttributeArgAST {
    enum class ArgKind { StringLit, IntLit, BoolLit, TypeIdent };
    ArgKind     argKind;
    std::string value;
    SourceLocation loc;
};
```

This is a minor issue compared to the others, but attribute arguments are currently invisible to the visitor. The semantic pass (`AttributeRegistry::validateAttribute`) iterates `attr.args` manually without any standard dispatch mechanism. This is acceptable for now since attribute args are structurally simple, but it should be noted as an inconsistency.

---

### Missing 5 — `FieldInitAST` is a plain struct with no visitor

```cpp
struct FieldInitAST {
    std::string  name;
    ExprPtr      value;
    SourceLocation loc;
};
```

`FieldInitAST` is owned by `StructLiteralExprAST`. The semantic pass must iterate over `inits` manually to type-check each field value. Since each init contains an `ExprPtr`, the expression itself is visitable — but the `FieldInitAST` wrapper (which carries the field name binding) is not. For a walker that needs to record "this expression was written for field X", it has no standard path.

This is lower priority than `SwitchCaseAST` and `TraitRefAST`, but consistent design would promote it.

---

## Summary Table

| # | File | Issue | Severity |
|---|---|---|---|
| Bug 1 | `DeclAST.hpp` | `FuncBodyKind` still exists but is now contradictory dead code | High |
| Bug 2 | `ExprAST.hpp` | `IfExprAST` comment says `StmtPtr` but fields are `ExprPtr` | Medium |
| Bug 3 | `ExprAST.hpp` | `FieldPatternAST` comment still justifies old `BaseAST*` design | Medium |
| Bug 4 | `ExprAST.hpp` | `MatchArmAST` comment still describes old `vector<unique_ptr<BaseAST>>` | Medium |
| Bug 5 | `ExprAST.hpp` | File-level `@note` block contradicts the actual new design | Medium |
| Bug 6 | `ExprAST.hpp` | `PipelineStepAST::anonFunc` has stale "forward declared below" comment | Low |
| Missing 1 | `StmtAST.hpp` | `ForStmtAST`/`ParallelForStmtAST` use `varName`+`varType` instead of `ParamPtr iterVar` | Medium |
| Missing 2 | `StmtAST.hpp` | `SwitchCaseAST` is not a `BaseAST` node — no visitor, no `ASTKind` | High |
| Missing 3 | `DeclAST.hpp` | `TraitRefAST` is not a `BaseAST` node — no visitor, no `ASTKind` | High |
| Missing 4 | `DeclAST.hpp` | `AttributeArgAST` is a plain struct invisible to the visitor | Low |
| Missing 5 | `ExprAST.hpp` | `FieldInitAST` is a plain struct invisible to the visitor | Low |
