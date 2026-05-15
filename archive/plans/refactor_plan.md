Looking at the codebase, here's what needs to change and why.

## Current Problem

The parser currently does three things it shouldn't:

1. **Qualifier parsing** — `parseFuncDecl`, `parseMethodDecl`, `parseTraitMethod` all call `QualifierRegistry::instance().applyQualifier()` and `allNames()` to validate `~async`, `~nullable`, `~parallel` at parse time.

2. **Attribute parsing** — `Parser.cpp` constructor caches `kw_extern`, `kw_inline`, etc. from `AttributeRegistry`. `parseVarDecl` and `parseFuncDecl` then inspect attribute names to make parsing decisions (e.g. whether to look for a body or skip it for `@extern`). `parseAttribute()` calls `validateAttribute()` implicitly via the registry.

3. **Intrinsic parsing** — The constructor caches `kw_sizeof` and `kw_alignof` from `IntrinsicRegistry`, and `parseIntrinsicCallExpr` uses them to decide whether to parse a type argument vs. value arguments.

The parser is making **semantic decisions** (is this a valid qualifier? does this extern function have a body?) that belong in the semantic phase.

---

## Refactor Plan

### Phase 1 — Qualifiers

**What to change:** Remove all `QualifierRegistry` calls from the parser. Instead of validating qualifier names, the parser should collect raw qualifier strings and store them as-is.

`FuncSignature` already has `rawQualifiers` (`std::vector<InternedString>`). The `qualifiers` bitmask field (`uint32_t`) is the semantic annotation. The parser should only populate `rawQualifiers` and leave `qualifiers = 0`. The semantic phase then resolves raw names to bits and reports unknown qualifiers.

**Specific changes:**

In `parseFuncDecl`, `parseMethodDecl`, `parseTraitMethod`, and `parseFuncType` — replace this pattern:

```cpp
if (!QualifierRegistry::instance().applyQualifier(qualifiersMask, qualRaw)) {
    errorAt(DiagCode::E2003, "unknown type qualifier ...");
}
node->sig.rawQualifiers.push_back(pool_.intern(qualRaw));
node->sig.qualifiers = qualifiersMask;
```

With just:

```cpp
node->sig.rawQualifiers.push_back(pool_.intern(qualRaw));
// qualifiers bitmask stays 0 — resolved by semantic phase
```

Remove the `qualifiersMask` local variable from all four functions. Remove the `#include "registry/QualifierRegistry.hpp"` from parser files once no other call sites remain.

The semantic pass (wherever `FuncSignature` is processed) gains a new helper, something like `resolveFuncSignatureQualifiers(FuncSignature&, DiagnosticEngine&)`, that iterates `rawQualifiers`, looks each up in `QualifierRegistry`, populates `sig.qualifiers`, and reports unknown names.

---

### Phase 2 — Intrinsics

**What to change:** Remove `kw_sizeof` and `kw_alignof` from `Parser` entirely, along with `IntrinsicRegistry` from the constructor and `parseIntrinsicCallExpr`.

The parser's only job in `parseIntrinsicCallExpr` is to figure out whether the first argument is a type or an expression. Currently it does this by checking if the intrinsic name is `sizeof`/`alignof`. But this is a semantic concern — the parser can't know which intrinsics exist.

The solution is to make the parser always attempt to parse the first argument as a type if it looks like one, and fall back to expression otherwise. This is already naturally handled by `looksLikeType()`. Replace:

```cpp
bool isTypeParam = (node->intrinsicName == kw_sizeof) ||
                   (node->intrinsicName == kw_alignof);
```

With a purely syntactic check:

```cpp
bool firstArgIsType = looksLikeType() && !check(TokenType::RPAREN);
```

This means the parser parses `#sqrt(float)` as a type argument too — but that's fine. The semantic phase validates whether the intrinsic accepts a type argument and will report an error if not. The AST structure (`typeArg` vs `args`) becomes a syntactic hint that the semantic pass corrects if needed.

Remove `kw_sizeof`, `kw_alignof`, and all `IntrinsicRegistry` includes/calls from `Parser.hpp` and `Parser.cpp`.

---

### Phase 3 — Attributes

This is the most involved change because attributes currently affect **parse-time control flow**, not just validation. Two separate concerns need separating:

**Concern A: Attribute validation** (easy) — `parseAttribute()` doesn't actually call `AttributeRegistry::validateAttribute()` directly, so there's no registry call to remove there. The validation happens elsewhere. This is already clean.

**Concern B: `@extern` driving parse decisions** (harder) — In `parseFuncDecl` and `parseVarDecl`, the parser checks `hasExternAttr` to decide whether to expect a body. This is the real problem. The logic branches like:

```cpp
if (hasExternAttr) {
    // don't parse a body
} else {
    consume(ASSIGN); parseBlock();
}
```

This couples attribute semantics into the grammar. The fix is to make the `=` and body **always optional** at parse time when a function or variable declaration is at top level. The parser should check for `=` without requiring it:

```cpp
if (check(TokenType::ASSIGN)) {
    advance();
    node->body = parseBlock() / parseExpr();
}
// body remains nullptr if no '=' present
```

The semantic phase then enforces: non-extern functions must have a body; extern functions must not. This is a semantic rule, not a grammar rule.

Remove `kw_extern`, `kw_packed`, `kw_inline`, `kw_noinline`, `kw_deprecated` from `Parser.hpp`. Remove `AttributeRegistry::instance()` calls from the constructor in `Parser.cpp`. Remove `#include "registry/AttributeRegistry.hpp"` from parser files.

The one remaining attribute-adjacent decision — in `parseTopLevelDecl`, distinguishing `@extern` var from `@extern` func — currently uses `hasParenAfterName` lookahead and is actually **independent** of the registry. It already does:

```cpp
bool hasParenAfterName = (tokens_[lookAhead].type == TokenType::LPAREN);
if (hasParenAfterName) { parseFuncDecl(...) } else { parseVarDecl(...) }
```

That lookahead is pure syntax and stays. The `@extern` name check that enables this branch can be replaced with a local string comparison against the raw interned name without any registry involvement — or better, the branch can be removed entirely since `looksLikeFuncDecl()` already handles this correctly for non-extern cases and can be extended.

---

### Phase 4 — Clean up `Parser.hpp`

Remove from the private section:

```cpp
InternedString kw_extern;
InternedString kw_packed;
InternedString kw_inline;
InternedString kw_noinline;
InternedString kw_deprecated;
InternedString kw_sizeof;
InternedString kw_alignof;
```

Remove from the constructor in `Parser.cpp` all the registry initialisation blocks and the assertions that fire if registry IDs are invalid.

---

### What moves to semantic phase

The semantic pass gains these new responsibilities:

- Resolve `sig.rawQualifiers` → `sig.qualifiers` bitmask for every `FuncSignature` encountered (functions, methods, trait methods, type aliases, anonymous functions).
- Validate that qualifier names are known, report unknown ones.
- Validate that `@extern` functions have no body (currently a parse error, becomes a semantic error).
- Validate that non-`@extern` functions do have a body.
- Validate that `@extern` variables have no initializer.
- Validate attribute contexts, argument counts, and mutual exclusions via `AttributeRegistry`.
- Determine whether an intrinsic's first argument is a type or value (the `typeArg` field in `IntrinsicCallExprAST` may need to become a `std::variant<TypePtr, ExprPtr>` or the semantic pass simply re-interprets it).

---

### Order of work

Do these in order to keep the build green at each step:

1. Qualifiers — smallest blast radius, self-contained, just stop populating the bitmask and move that to semantic.
2. Intrinsics — change `parseIntrinsicCallExpr` to use `looksLikeType()`, remove the two cached IDs.
3. Attributes (body optionality) — make `=` optional in `parseFuncDecl`, move body-presence enforcement to semantic.
4. Attributes (constructor cleanup) — remove cached IDs and registry assertions.
5. Remove includes from parser files once all call sites are gone.