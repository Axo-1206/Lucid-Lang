# Parser Static Analysis Report

> [!NOTE]
> All AST nodes are arena-allocated — there are **no raw `new`/`delete` memory leaks** in the classical sense. The risks below are logic bugs, unbounded loops, dangling references, and use-before-init patterns.

---

## 🔴 HIGH — Potential Infinite Loops

### 1. `parseArgList` — no progress guard (ParserExpr.cpp:1455)

```cpp
while (!check(TokenType::RPAREN) && !isAtEnd()) {
    ExprPtr arg = parseExpr();
    if (!arg) {
        errorAt(...);
        break;  // ← breaks on null, BUT...
    }
    ...
}
```

**Problem:** `parseExpr()` never returns `nullptr` — it always returns at least `UnknownExprAST`. So the `!arg` guard **never fires**. If `parseExpr()` makes no progress (e.g., malformed input where the current token is not consumable), the loop **spins forever**.

**Compare:** `parseArrayLiteralExpr` (line 921) has the correct pattern:
```cpp
size_t beforePos = pos_;
ExprPtr elem = parseExpr();
if (pos_ == beforePos) { advance(); break; } // ← correct
```

**Fix:** Add a `savedPos` / `pos_ == savedPos` guard before the `parseExpr()` call in `parseArgList`.

---

### 2. `parseStructPattern` — no progress guard (ParserExpr.cpp:2164)

```cpp
while (!check(TokenType::RBRACE) && !isAtEnd()) {
    match(TokenType::COMMA);
    if (check(TokenType::RBRACE)) break;

    FieldPatternPtr fp = parseFieldPattern();
    if (fp)
        pat->fields.push_back(std::move(fp));
    // ← no advance if fp == nullptr
}
```

**Problem:** If `parseFieldPattern()` returns `nullptr` (e.g., the current token is not `IDENTIFIER`), the loop does not advance — spinning forever on the same bad token.

**Fix:** Add `else { advance(); }` or a saved-position check after the `if (fp)` branch.

---

### 3. `parseAttributes` — potentially unbounded if `parseAttribute` returns null without advancing (ParserDecl.cpp:81)

```cpp
while (check(TokenType::AT_SIGN)) {
    AttributePtr attr = parseAttribute();
    if (attr) {
        attrs.push_back(std::move(attr));
    }
    // ← no advance if parseAttribute returned nullptr
}
```

`parseAttribute` returns `nullptr` only after consuming `@` AND finding no `IDENTIFIER`. But at that point `pos_` **has** advanced past `@` (line 98: `advance()`), so `check(TokenType::AT_SIGN)` will be false on the next iteration — no loop. This is **safe**, but fragile: if the control flow inside `parseAttribute` changes, this becomes a spin-loop.

---

### 4. `parseDefaultArm` infinite loop risk (ParserExpr.cpp:1893)

```cpp
while (true) {
    size_t savedPos = pos_;
    ExprPtr exp = parseExpr();
    if (pos_ == savedPos) {
        if (!arm->exprs.empty()) break;  // ← exits
        errorAt(...);
        break;                            // ← also exits
    }
    arm->exprs.push_back(std::move(exp));
    if (!match(TokenType::COMMA)) break;
}
```

This is correctly guarded. ✅ But if `match(TokenType::COMMA)` always succeeds (malformed token stream with repeated commas), and each `parseExpr()` call advances (returning `UnknownExprAST`), you get unbounded expression parsing. Low risk, but worth a max-iteration guard.

---

## 🟠 MEDIUM — Logic Bugs / Incorrect Behavior

### 5. `looksLikeAnonFunc` — incorrect check after `parseOneGroup` (Parser.cpp:569–575)

```cpp
i = parseOneGroup(i);
if (i >= tokens_.size() || tokens_[i].type != TokenType::RPAREN) {
    // The helper returns after the ')', so the current token should be something else.
    // Actually parseOneGroup returns index after the ')', so we don't need to check for ')'.
    // But we must ensure the group was properly closed.
    // We'll simply continue.
}
```

**Problem:** The comment itself admits `parseOneGroup` already returns the index **after** the closing `)`. Yet the condition `tokens_[i].type != TokenType::RPAREN` checks for a `)` that should **not** be there. The entire `if` block is a dead/misleading no-op. The real risk: if `parseOneGroup` didn't find a closing `)` and returned `tokens_.size()`, the subsequent `while` loop could also be skipped silently, causing `looksLikeAnonFunc` to return `true` on a malformed token stream.

---

### 6. `parseTopLevelDecl` — attrs moved into `parseFuncDecl` but NOT into `parseVarDecl` for the non-extern path (Parser.cpp:928–930)

```cpp
if (isFunc) {
    return parseFuncDecl(kw, vis, std::move(attrs));  // attrs transferred ✅
} else {
    auto decl = parseVarDecl(vis);        // attrs NOT passed ❌
    if (decl) decl->attributes = std::move(attrs);   // attached after
    return decl;
}
```

If `parseVarDecl` returns `nullptr` (error path), `attrs` is destroyed without being used. That's correct behavior, but the **parallel @extern path** at line 915 correctly passes `attrs` directly. This inconsistency means the validation inside `parseVarDecl` (checking `@packed`, `@inline`, etc.) runs against an **empty** attrs list when called from the non-extern branch; the validation is then duplicated by the outer post-assignment `decl->attributes = std::move(attrs)`. The attribute checking inside `parseVarDecl` (lines 345–358) therefore **never fires** for the non-extern branch.

---

### 7. `parseEnumVariant` — `strtoll` narrows to `int` silently (ParserDecl.cpp:916–919)

```cpp
long val = std::strtoll(raw.c_str(), &endPtr, base);
...
variant->explicitValue = static_cast<int>(val);
```

**Problem:** `strtoll` returns `long` (or `long long`), but it's cast to `int`. For large enum values common in Vulkan bitmasks (e.g., `0x80000000`), this silently truncates. The overflow is undefined behavior on signed `int` overflow.

**Fix:** Use `static_cast<int64_t>(val)` and store it in an `int64_t` field, or use `strtoul` with proper range checking.

---

### 8. `harvestDocComment` — line number `0` is used as a sentinel but `<= 0` check is applied only to comment tokens (Parser.cpp:270, 309)

```cpp
if (t.line <= 0) continue;  // for LINE_COMMENT
if (t.line <= 0) continue;  // for DOC_COMMENT
```

**Problem:** The loop at line 264 starts at `pos_` and walks backward, but the termination condition only breaks on non-comment tokens. If the token stream contains many comment tokens with `line == 0` (invalid), the `continue` statements make the loop iterate back through all of them without ever hitting a real token or the loop start (`i > 0`). This is O(n) rather than O(1) per harvest call, but more importantly the logic never accumulates those tokens properly.

---

### 9. `parseFuncType` — `isNullableFunction` can consume the outer `(` but then fail the inner `consume(LPAREN)` (ParserType.cpp:546–551)

```cpp
if (parenDepth == 0 && hasQuestion) {
    isNullableFunction = true;
    advance(); // consume the outer '('     ← pos_ now inside
}

// ── Parse the actual function type (inner part) ───
consume(TokenType::LPAREN, "expected '(' for function type");  // ← must find another '('
```

**Problem:** The outer `(` is consumed (line 546), but then `consume(LPAREN)` is immediately called expecting **another** `(`. This only works if the form is `((params) ret)?`, i.e., the inner content starts with `(`. If the nullable detection fires incorrectly (e.g., for a form like `(int)?`), the parser will report a spurious "expected '('" error and misparse the type. The lookahead logic correctly checks for nested parens, but a nested function type `((int) string)?` will correctly match while a simple nullable primitive that happens to follow `(` might not — the disambiguation is subtle and could break on edge cases.

---

### 10. `IntrinsicRegistry::setStringPool` — guarded with `if (stringPool) return` but `AttributeRegistry::setStringPool` always rebuilds (IntrinsicRegistry.cpp:316 vs AttributeRegistry.cpp:70)

```cpp
// IntrinsicRegistry:
void IntrinsicRegistry::setStringPool(StringPool& pool) {
    if (stringPool) return;  // ← silently skips if called twice
    ...
}

// AttributeRegistry:
void AttributeRegistry::setStringPool(StringPool& pool) {
    // Always rebuild from the static array (even if already set, to allow re-init).
    ...
}
```

**Problem:** Inconsistent design. If a test harness (or future multi-compilation-unit support) calls `setStringPool` more than once, `IntrinsicRegistry` silently uses the **old** pool while `AttributeRegistry` re-initializes. This leads to dangling `string_view` inside `AttributeInfo::name` (which points into the old pool's memory) after a re-init, while `IntrinsicRegistry` remains consistent with the old pool. The `resetStringPool()` / `setStringPool()` cycle would work, but the asymmetry is a latent bug.

---

## 🟡 LOW — Code Quality / Minor Risks

### 11. `parseParamGroup` — `savedPos` is overwritten before use (ParserDecl.cpp:602–614)

```cpp
size_t savedPos = pos_;           // L602: before advance() calls
...
InternedString paramName = pool_.intern(advance().value);  // consumes name
bool isVariadic = match(TokenType::VARIADIC);
savedPos = pos_;                  // L614: ← OVERWRITES the original savedPos
TypePtr paramType = parseType();
if (paramType->isa<UnknownTypeAST>() && pos_ == savedPos) {
```

The first assignment of `savedPos` at L602 is **dead code** — it is overwritten at L614 before being read. The intent is to detect if `parseType()` consumed no tokens, which is correctly captured at L614. The L602 assignment should be removed or the comment updated to prevent confusion.

---

### 12. Unused `anonFunc` variable in `looksLikeAnonFunc` check (Parser.cpp:583)

```cpp
bool anonFunc = (n2 == TokenType::LBRACE) || isTypeStart(n2); // computed
if (n2 == TokenType::LBRACE || isTypeStart(n2)) {             // repeated
    return parseAnonFuncExpr();
}
```

`anonFunc` is computed but never used in the condition — the condition is duplicated verbatim. Compiler likely warns about this unused variable.

---

### 13. `AttributeInfo::name` is `std::string_view` pointing into `StringPool` (AttributeRegistry.cpp:82)

```cpp
std::string_view nameView = pool.lookup(id);
info.name = nameView;
```

`std::string_view` is a non-owning reference. If the `StringPool` is destroyed or its internal buffer is reallocated, `info.name` becomes a dangling reference. This is safe as long as the pool outlives the registry, but there is no lifetime annotation or `[[lifetimebound]]` attribute enforcing this contract.

---

### 14. `parseGenericArgs` — infinite loop risk on malformed input (ParserType.cpp:690–708)

```cpp
do {
    match(TokenType::COMMA);
    if (check(TokenType::GREATER)) break;

    TypePtr arg = parseType();
    if (!arg) { errorAt(...); break; }  // ← BUT parseType() never returns nullptr
    args.push_back(std::move(arg));
    ...
} while (!check(TokenType::GREATER) && !isAtEnd());
```

`parseType()` always returns a valid pointer (worst case `UnknownTypeAST`). So `!arg` never fires. If `parseType()` returns `UnknownTypeAST` and makes no token progress, the `do...while` spins indefinitely. A `savedPos` guard is needed here.

---

## Summary Table

| # | Severity | File | Location | Category |
|---|----------|------|----------|----------|
| 1 | 🔴 HIGH | ParserExpr.cpp | `parseArgList` L1455 | Infinite loop |
| 2 | 🔴 HIGH | ParserExpr.cpp | `parseStructPattern` L2164 | Infinite loop |
| 3 | 🔴 HIGH | ParserType.cpp | `parseGenericArgs` L690 | Infinite loop |
| 4 | 🟠 MED | Parser.cpp | `looksLikeAnonFunc` L569 | Logic bug |
| 5 | 🟠 MED | Parser.cpp | `parseTopLevelDecl` L928 | Attr validation skipped |
| 6 | 🟠 MED | ParserDecl.cpp | `parseEnumVariant` L919 | UB / silent truncation |
| 7 | 🟠 MED | Parser.cpp | `harvestDocComment` L270 | Perf / logic gap |
| 8 | 🟠 MED | ParserType.cpp | `parseFuncType` L546 | Nullable detection edge case |
| 9 | 🟠 MED | Registry files | `setStringPool` | Inconsistent re-init |
| 10 | 🟡 LOW | ParserDecl.cpp | `parseParamGroup` L602 | Dead code |
| 11 | 🟡 LOW | Parser.cpp | `looksLikeAnonFunc` L583 | Unused variable |
| 12 | 🟡 LOW | AttributeRegistry.cpp | `AttributeInfo::name` | Dangling `string_view` risk |
