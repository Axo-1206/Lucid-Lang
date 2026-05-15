# Parser Bug Report — Pass 2 (Post-Fix Scan)

> [!NOTE]
> All round-1 bugs confirmed fixed. The issues below are **newly identified** from a fresh
> full-file read of the post-fix codebase. Severity is assessed independently.

---

## 🔴 HIGH — Potential Infinite Loops

### 1. `parseAttribute` — arg-list loop has no progress guard (ParserDecl.cpp:118)

```cpp
if (match(TokenType::LPAREN)) {
    while (!check(TokenType::RPAREN) && !isAtEnd()) {
        match(TokenType::COMMA);         // optional
        if (check(TokenType::RPAREN)) break;

        // ← branches: STRING_LITERAL, INT_LITERAL, IDENTIFIER, TRUE, FALSE ...
        // ← ELSE branch:
        else {
            errorAt(..., "attribute argument must be ...");
            while (!check(TokenType::RPAREN) && !isAtEnd()) advance();  // inner recovery
            break;
        }
    }
```

**Problem:** The outer loop correctly breaks in the `else` branch. However, the recognized branches (e.g., `STRING_LITERAL`, `INT_LITERAL`) all call `advance()` via the token consumption, so they **do** make progress. The hidden risk is: if `match(TokenType::COMMA)` at line 119 **consumes** a comma and the next token is again a comma (`@foo(,,)`), the outer loop continues but hits a comma again. The comma is consumed by `match(COMMA)`, nothing matches the arg type, and the `else` fires and breaks. So **this is safe**, but fragile.

**Real risk:** If a future recognized arg kind is added that does *not* always call `advance()` (e.g., an optional sub-expression), the loop becomes an infinite spin. A `savedPos` guard would make this loop permanently safe.

---

### 2. `parseStructLiteralExpr` — no progress guard after `synchronize()` (ParserExpr.cpp:955–983)

```cpp
while (!check(TokenType::RBRACE) && !isAtEnd()) {
    ...
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(DiagCode::E2003, "expected field name in struct literal");
        synchronize();                          // ← may return at RBRACE
        if (check(TokenType::RBRACE) || isAtEnd())
            break;
        continue;                               // ← loops back
    }
    std::string fieldName = advance().value;
    consume(TokenType::ASSIGN, ...);
    ExprPtr val = parseExpr();
    if (!val) {
        errorAt(...);
        continue;                               // ← no advance here!
    }
```

**Problem:** If `parseExpr()` returns a non-null `UnknownExprAST` (which it always does) **but makes no token progress**, `val` is non-null and the loop continues with the same token. This is a silent infinite loop because `!val` never fires. 

**Fix:** Add a `savedPos` / `pos_ == savedPos` guard around the `parseExpr()` call, the same pattern used in `parseArgList`.

---

### 3. `parseSwitchCase` first value — no progress guard (ParserStmt.cpp:394)

```cpp
} else {
    ExprPtr val = parsePrattExpr(0);   // ← no savedPos check
    if (val) {
        if (check(TokenType::RANGE)) {
            sc->values.push_back(parseRangeExpr(std::move(val)));
        } else {
            sc->values.push_back(std::move(val));
        }
    }
}
```

The **additional values** loop (line 409) has the correct `savedPos` guard. But the **first value** (line 394) does not. If `parsePrattExpr(0)` returns `UnknownExprAST` with no token consumed, the caller (`parseSwitchStmt`, line 330) loops back, calls `parseSwitchCase` again, and the same stuck token is processed in an outer loop that **does** have a `synchronize()` call but only on "unrecognized" tokens — not on `CASE`. If the stuck token happens to be `CASE`, this is an infinite loop.

**Fix:** Save `pos_` before calling `parsePrattExpr(0)` on the first value and check progress.

---

## 🟠 MEDIUM — Logic Bugs / Incorrect Behavior

### 4. `parseFuncDecl` — `@extern` body-error path does not consume the `=` and body (ParserDecl.cpp:521–526)

```cpp
if (hasExternAttr) {
    ...
    } else if (check(TokenType::ASSIGN)) {
        errorAt(DiagCode::E2002, "'@extern' function must not have a body");
        // ← NO advance, NO body consumption
    } else {
        // warning: missing semicolon (silent)
    }
    return node;
}
```

**Problem:** When `@extern` is detected and a `=` is found (body present), an error is reported but **the `=` and the entire function body `{ ... }` are NOT consumed**. The parser returns the `FuncDeclAST`, then the outer `parseTopLevelDecl` loop sees `=` as the next token, which it does not recognize as a declaration start, so it calls `synchronize()` — but `synchronize()` does not stop at `=` (it's not in the boundary set). The result is that the body tokens are consumed one-by-one, potentially causing cascading parse errors for the entire rest of the file.

**Fix:** After reporting the error, consume the `=` and then call `parseBlock()` (discarding the result) to recover cleanly.

---

### 5. `parseMethodDecl` — anon-func form body discards the parsed return type (ParserDecl.cpp:1233–1244)

```cpp
} else if (check(TokenType::LPAREN)) {
    parseParamGroup(); // Consume the repeated param group
    if (looksLikeType() && !check(TokenType::LBRACE)) {
        parseType();    // ← return type is parsed but NOT stored
    }
    if (!check(TokenType::LBRACE)) { ... }
    method->body = parseBlock();
```

**Problem:** When a method body is written in anon-func form (`= (params) ret { ... }`), the repeated return type is parsed via `parseType()` but its result is **discarded**. Normally the signature's return type (`method->sig.returnType`) is already set from the declaration header. If the repeated type differs from the header type, the mismatch is silently ignored — the semantic pass will never know. This is also a pattern in `parseFuncDecl` (line 549–551), so it is consistent — but it should be at least warned about if it doesn't match the declared type.

---

### 6. `parseParamGroup` — infinite loop if `parseType` makes no progress but returns non-Unknown (ParserDecl.cpp:624)

```cpp
savedPos = pos_;
TypePtr paramType = parseType();
if (paramType->isa<UnknownTypeAST>() && pos_ == savedPos) {
    errorAt(DiagCode::E2005, "expected parameter type");
    break;
}
```

**Problem:** The progress guard fires **only if** `parseType()` returns `UnknownTypeAST` **AND** made no progress. However, `parseType()` may return a non-Unknown type (e.g., a `PrimitiveTypeAST`) on a token that it does consume, meaning the guard is correct in that path. But if `parseType()` returns `UnknownTypeAST` and **did** advance (consuming an unrecognized token and producing the unknown node), the guard is bypassed and the loop continues to the next iteration — **good**, that's correct.

The real edge: if `parseBaseType()` hits the `default:` case and returns `UnknownTypeAST` **without** advancing (since `default` just constructs the node, it does NOT call `advance()`), AND `pos_ == savedPos`, then the break fires. ✅ So this is safe as written.

However, there's a subtle issue: the guard will **not** fire when `parseType()` returns `UnknownTypeAST` while `pos_ != savedPos` (i.e., it consumed a token but still failed). In that case, the param is added with an `UnknownTypeAST` type, which will cause downstream issues in the semantic pass. Consider checking just `paramType->isa<UnknownTypeAST>()` without the position requirement, and emitting a warning.

---

### 7. `parseFromDecl` — `synchronize()` inside `while` loop may leave loop in unstable state (ParserDecl.cpp:1305–1364)

```cpp
while (!check(TokenType::RBRACE) && !isAtEnd()) {
    ...
    if (!check(TokenType::LPAREN)) {
        errorAt(...);
        synchronize();   // ← may stop at RBRACE (correct) or at a statement keyword (wrong)
        continue;        // ← continues the while loop
    }
    ...
    if (!check(TokenType::IDENTIFIER)) {
        errorAt(...);
        synchronize();
        continue;
    }
    ...
    if (!check(TokenType::ASSIGN)) {
        errorAt(...);
        synchronize();
        continue;
    }
```

**Problem:** `synchronize()` stops at top-level declaration keywords (`LET`, `CONST`, `STRUCT`, etc.) and `RBRACE`. If it stops at `LET` (e.g., the developer forgot the body of a from-entry and the next token is a top-level declaration), the `while` loop continues and attempts to parse `LET` as a `from_entry`, which expects `LPAREN`. The error fires again, `synchronize()` is called again from `LET`, and depending on what follows, this could spiral into repeated cascading errors without the parser making forward progress through the `from` block.

**Fix:** After `synchronize()`, check if the current token is a known top-level declaration starter (not just `RBRACE`), and if so, `break` out of the `while` loop to let the outer parser handle it.

---

### 8. `parseMatchExpr` — no progress guard around first call to `parseMatchArm` inside loop (ParserExpr.cpp:1213–1226)

```cpp
size_t beforePos = pos_;
MatchArmPtr arm = parseMatchArm();
if (pos_ == beforePos) {
    errorAt(DiagCode::E2007, "failed to parse match arm, skipping");
    synchronize();
    if (check(TokenType::RBRACE) || isAtEnd())
        break;
    continue;
}
```

This is correctly guarded. ✅

However, `parseMatchArm` itself calls `parsePattern()` which may return `nullptr` (line 1773), in which case `parseMatchArm` returns `nullptr` (line 1773–1774). But the `beforePos` check at the call site does NOT check for `arm == nullptr` separately — it only checks `pos_ == beforePos`. If `parsePattern()` returns `nullptr` after consuming tokens (e.g., consumed the pattern identifier but failed on `is`), `arm` is `nullptr` but `pos_ != beforePos`, so no error/synchronize is called. An arm with `nullptr` is silently dropped. **This is not an infinite loop but is a silent data loss** — the pattern may have been partially consumed, leaving the token stream misaligned.

---

### 9. `looksLikeAnonFunc` — `VARIADIC` token not skipped in `parseOneGroup` (Parser.cpp:551–568)

```cpp
auto parseOneGroup = [&](std::size_t start) -> std::size_t {
    ...
    while (j < tokens_.size() && parenDepth > 0) {
        TokenType tt = tokens_[j].type;
        if (tt == TokenType::LPAREN)  { ++parenDepth; }
        else if (tt == TokenType::RPAREN) { --parenDepth; }
        else if (tt == TokenType::LINE_COMMENT || tt == TokenType::DOC_COMMENT) { ++j; continue; }
        else if (tt == TokenType::TILDE) { /* skip qualifier */ }
        // ← VARIADIC (...) not special-cased
        ++j;
    }
```

**Problem:** The `VARIADIC` token (`...`) is a three-character token that does not contain `(` or `)`. This is fine — it is just incremented past like any other token. ✅ **No bug here for `VARIADIC` itself**.

But the `TILDE` handling is inconsistent: the code increments `j` at the end of the loop body (`++j`), **then** for `TILDE` it also does `++j` inside the branch for the identifier. So a `~async` inside a paren group actually increments `j` by 2 for the `~` and then `++j` runs the third time at end of iteration — it skips **3** indices (`~`, `async`, and the token after `async`). This causes `parseOneGroup` to skip the token **after** the qualifier name, potentially skipping a `,` or `)` inside the group and causing the depth tracking to become incorrect.

**Fix:** Use `continue` after the `++j` inside the `TILDE` branch to skip the bottom `++j`, or restructure as `else if` with an explicit `continue`.

---

## 🟡 LOW — Code Quality / Minor Risks

### 10. `parseDefaultArm` — allows arbitrary number of commas / expressions (ParserExpr.cpp:1893–1918)

The grammar comment says "at most two expressions", and `parseMatchArm` enforces this limit (lines 1849–1872). However `parseDefaultArm` has **no such limit** — the `while (true)` loop with `match(COMMA)` at the end will keep accepting `expr , expr , expr , ...` indefinitely. This is likely an oversight when the two-expression limit was added to match arms.

---

### 11. `parseVarDecl` — `kw_*` interned strings may be invalid if `setStringPool` was never called (ParserDecl.cpp:347–363)

```cpp
InternedString packedStr = kw_packed;
InternedString inlineStr = kw_inline;
// ...
for (const auto& attr : attrs) {
    if (attr->name == packedStr) { ... }
```

`kw_packed`, `kw_inline` etc. are initialized in the `Parser` constructor via `AttributeRegistry::instance().getPackedId()`. If `setStringPool()` was never called (e.g., in a unit test that constructs a `Parser` before calling `setStringPool`), the `AttributeRegistry` returns uninitialized `InternedString` values. The comparison `attr->name == packedStr` would then be comparing against a zero/default-constructed ID, which would match no real attribute — silent false negatives rather than a crash. Defensive: add an `assert(kw_packed.isValid())` guard.

---

### 12. `parseParallelBlockStmt` — `synchronize()` inside `parallelDepth_` is elevated (ParserStmt.cpp:820)

```cpp
++parallelDepth_;
while (!check(TokenType::RBRACE) && !isAtEnd()) {
    if (!check(TokenType::LBRACE)) {
        errorAt(...);
        synchronize();   // ← called while parallelDepth_ > 0
        continue;
    }
    node->subBlocks.push_back(parseBlock());
}
--parallelDepth_;
```

**Problem:** `synchronize()` itself does not call any statement parsers, so `parallelDepth_` doesn't directly affect it. However, if `synchronize()` stops at `RETURN`, `BREAK`, or `CONTINUE`, then the next iteration of the `while` loop will try to call `parseBlock()` on those tokens, fail to find `{`, call `synchronize()` again... This is not an infinite loop because `synchronize()` advances past the problem token, but it does produce redundant error messages. The actual risk: if `synchronize()` sees `RETURN` and returns, the outer `while` loop calls `errorAt("expected '{'")` again, then `synchronize()` again. Eventually EOF or `RBRACE` is reached. **Low severity but produces noisy diagnostic output.**

---

## Summary Table

| # | Severity | File | Location | Category |
|---|----------|------|----------|----------|
| 1 | 🔴 HIGH | ParserExpr.cpp | `parseStructLiteralExpr` L955 | Infinite loop (silent) |
| 2 | 🔴 HIGH | ParserStmt.cpp | `parseSwitchCase` first value L394 | Infinite loop risk |
| 3 | 🟠 MED | ParserDecl.cpp | `parseFuncDecl` @extern + body L521 | Body not consumed on error |
| 4 | 🟠 MED | ParserDecl.cpp | `parseMethodDecl` anon-form L1233 | Return type silently discarded |
| 5 | 🟠 MED | ParserDecl.cpp | `parseFromDecl` `synchronize()` L1305 | Loop not broken on decl boundary |
| 6 | 🟠 MED | ParserExpr.cpp | `parseMatchArm`/`parsePattern` null arm | Silent token misalignment |
| 7 | 🟠 MED | Parser.cpp | `looksLikeAnonFunc` TILDE branch L560 | Off-by-one `++j` skips wrong token |
| 8 | 🟡 LOW | ParserDecl.cpp | `parseAttribute` arg-list L118 | No progress guard (fragile) |
| 9 | 🟡 LOW | ParserExpr.cpp | `parseDefaultArm` no expr limit | Allows >2 result expressions |
| 10 | 🟡 LOW | ParserDecl.cpp | `parseVarDecl` kw_* validity L347 | Silent false negative if pool unset |
| 11 | 🟡 LOW | ParserStmt.cpp | `parseParallelBlockStmt` L820 | Noisy diagnostics on recovery |
