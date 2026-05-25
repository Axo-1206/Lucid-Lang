Let me read the updated parser files to check current diagnostic usage and comment-skipping.Now I have a thorough picture. Let me compile the full analysis.

---

## Analysis: Syntax Diagnostic Codes (E2000–E2999)

### 1. Critical Bug: `consume()` Never Reports Errors

This is the most fundamental problem. `TokenStream::consume()` silently returns a dummy token on mismatch — it **never calls `errorAt()` or `dc_.error()`**:

```cpp
Token TokenStream::consume(TokenType type, DiagCode code, const std::string& msg) {
    if (check(type)) return advance();
    return {type, "", 0, 0};   // ← silent failure, msg and code are ignored
}
```

This means every `ts_.consume(...)` call throughout the parser — all 101 of them — **produces no diagnostic when the expected token is absent**. The `msg` and `DiagCode` parameters are dead parameters. The fix is:

```cpp
Token TokenStream::consume(TokenType type, DiagCode code, const std::string& msg) {
    if (check(type)) return advance();
    // report the error using the parser's engine, then return dummy
    dc_.error(DiagnosticCategory::Syntax, filePath_, currentLoc(), code, {msg});
    return {type, "", 0, 0};
}
```

Since `TokenStream` doesn't currently hold a `DiagnosticEngine&`, this requires either passing it at construction or having the parser's `error()` method be callable from there. The cleanest fix is to give `TokenStream` a pointer/reference to the engine and file path at construction.

---

### 2. Comment Skipping: Documented But Not Implemented

The header and comments say `advance()` and `peek()` "transparently skip comments", but the actual implementation does **not**:

```cpp
// Parser.hpp
const Token& peek() const { return tokens_[pos_]; }   // no skipping
Token advance() { return tokens_[pos_++]; }             // no skipping
```

Comments are NOT being skipped by the core stream — the `pos_` simply increments by 1 each time. If the lexer emits `LINE_COMMENT` and `DOC_COMMENT` tokens into the stream, the parser will treat them as real tokens and misparse everything.

The only actual comment-skipping infrastructure is `skipCommentsFrom()` (a lookahead-only helper) and the manual loops in `ParserLookahead.cpp`. Those are correct for lookahead, but the normal grammar path through `peek()`/`check()`/`advance()`/`match()` does not skip comments at all.

**Fix:** Either (a) have `advance()` and `peek()` skip comment tokens, or (b) have the lexer strip comments before building the token vector so they never enter the stream. Option (b) is simpler and avoids the complication of `getPos()`/`setPos()` skipping over variable numbers of tokens.

---

### 3. Unused Syntax Codes and What Should Trigger Them

The following 15 codes are defined but **never emitted** by the parser:

| Code    | Description                            | Where it should fire                                                 | Current behavior                                                    |
| ------- | -------------------------------------- | -------------------------------------------------------------------- | ------------------------------------------------------------------- |
| `E2004` | Expected `in` in for-loop              | `ts_.consume(TokenType::IN, ...)` in `parseForStmt`                  | Uses no-code overload → silent                                      |
| `E2011` | Wrong argument count for `@` attribute | `parseAttribute()` — never validates arg count                       | Attribute arg count not checked at all                              |
| `E2012` | Unexpected keyword                     | `parseDeclaration()` default case                                    | Uses `E2002` instead                                                |
| `E2016` | `?` directly on inline function type   | `parseTypeWithNullable()` after `parseFuncType()`                    | `?` silently accepted on `FuncTypeAST`                              |
| `E2017` | Multiple param groups after `->`       | `parseReturnList()` / `parseFuncType()`                              | Not validated                                                       |
| `E2018` | Missing `->` in from entry             | `parseFromDecl()` when arrow missing                                 | Uses `E2001`                                                        |
| `E2019` | Missing `=` before function body       | `parseFuncDecl`, `parseMethodDecl`                                   | Uses `E2001`                                                        |
| `E2020` | `!` only allowed in pipeline step      | `parsePipelineStep()` when bang detected outside pipeline            | Uses `E2001`                                                        |
| `E2021` | Nullable chain `?.` missing `??`       | `parsePostfixExpr()` after building `NullableChainExprAST`           | Not validated at all — chain with no `??` is silently accepted      |
| `E2022` | `default` arm not last in match        | `parseMatchExpr()` when default appears mid-arms                     | Not enforced — `break` stops processing but no error if arms follow |
| `E2024` | Method not found on receiver           | Could be parse-time for `::`                                         | Semantic-phase concern, appropriate to leave                        |
| `E2025` | Field access on non-struct             | Semantic concern                                                     | Appropriate to leave                                                |
| `E2026` | Chained comparison                     | `parseInfixBinary()`                                                 | **Uses `E3014` instead** (semantic code used in parser)             |
| `E2028` | Invalid type in result suffix          | `parseTypeWithNullable()` after `!`                                  | `parseType()` reports `E2005` or accepts wrong things               |
| `E2029` | Nested `!` not allowed                 | `parseTypeWithNullable()` when `errorType` itself is `ResultTypeAST` | Not checked at all                                                  |

The actionable ones (parser can detect at parse time) are detailed below.

---

### 4. Per-Code Fixes

**E2004** — `parseForStmt` line 808:
```cpp
// current (no DiagCode):
ts_.consume(TokenType::IN, "expected 'in'");
// fix:
ts_.consume(TokenType::IN, DiagCode::E2004, "expected 'in' in for-loop");
```

**E2012** — `parseDeclaration()` default error branch currently uses `E2002`. `E2002` means "token not allowed in this context"; `E2012` is specifically "unexpected keyword". If the offending token is a keyword, emit `E2012`; otherwise `E2002` is correct.

**E2016** — `parseTypeWithNullable()` should detect when the base type is a `FuncTypeAST` before accepting `?`:
```cpp
if (ty && ts_.match(TokenType::QUESTION)) {
    if (ty->isa<FuncTypeAST>()) {
        errorAt(DiagCode::E2016, "'?' is not valid directly on an inline function type; use a type alias");
        // don't wrap in NullableTypeAST — or wrap it anyway for recovery
    } else {
        ty = arena_.make<NullableTypeAST>(std::move(ty));
    }
}
```

**E2018** — `parseFromDecl()` line 1411:
```cpp
// current:
errorAt(DiagCode::E2001, "expected '->' before return type for conversion entry");
// fix:
errorAt(DiagCode::E2018, "expected '->' before return type for conversion entry");
```

**E2019** — `parseMethodDecl()` line 1161 and `parseFuncDecl()` implicit case, and `parseFromDecl()` line 1425:
```cpp
// current:
errorAt(DiagCode::E2001, "expected '=' before method body");
// fix:
errorAt(DiagCode::E2019, "expected '=' before method body");
```

**E2020** — In `parseBehaviorPipelineStep`, `parseFieldPipelineStep`, `parseArgPackPipelineStep`, and `parseIndexPipelineStep`, when `!` is missing after an arg-pack it uses `E2001`. Change those four sites to `E2020`. Also, the `!` appearing on an anonymous function should emit `E2020` (grammar forbids it):
```cpp
// In parseAnonFuncPipelineStep or wherever anon funcs reach the ! check:
errorAt(DiagCode::E2020, "'!' is forbidden on anonymous function pipeline steps");
```

**E2021** — After building `NullableChainExprAST` in `parsePostfixExpr`, check that the next infix operator is `??`:
```cpp
lhs = std::move(chain);
// The grammar mandates ?? follows a ?. chain
if (!ts_.check(TokenType::QUESTION_QUESTION) && /* not at statement end */ !ts_.check(TokenType::RBRACE) ...) {
    errorAt(DiagCode::E2021, "nullable chain '?.' must be terminated by '??'");
}
continue;
```

**E2022** — `parseMatchExpr()` after `break`ing out of the default arm, check whether there are still non-`}` tokens remaining:
```cpp
hasDefault = true;
break;  // current: just break
// fix: after the loop
if (hasDefault && !ts_.check(TokenType::RBRACE)) {
    errorAt(DiagCode::E2022, "'default' arm must be last in match expression");
    // consume remaining arms as recovery
}
```

**E2026** — `parseInfixBinary()` line 379 emits `E3014` (a semantic-range code) from the parser. Move it to the correct syntax code:
```cpp
// current:
errorAt(DiagCode::E3014, "chained comparisons not allowed; use 'and' explicitly");
// fix:
errorAt(DiagCode::E2026, "chained comparisons not allowed; use 'and' explicitly");
```
(`E3014` should be reserved for the semantic pass to catch cases the parser cannot catch.)

**E2028** — `parseTypeWithNullable()` should validate the error type after `!` is a named/primitive type and not an array or inline function:
```cpp
if (looksLikeType()) {
    errorType = parseType();
    if (errorType && (errorType->isa<SliceTypeAST>() || errorType->isa<DynamicArrayTypeAST>() ||
                      errorType->isa<FixedArrayTypeAST>() || errorType->isa<FuncTypeAST>())) {
        errorAt(DiagCode::E2028, "invalid type in result suffix: expected primitive or identifier");
    }
}
```

**E2029** — In `parseTypeWithNullable()`, after parsing `errorType`, check it is not itself a `ResultTypeAST`:
```cpp
if (errorType && errorType->isa<ResultTypeAST>()) {
    errorAt(DiagCode::E2029, "nested '!' not allowed in type; use a type alias");
}
// Also check the success type (ty before the !):
if (ty->isa<ResultTypeAST>()) {
    errorAt(DiagCode::E2029, "nested '!' not allowed in type; use a type alias");
}
```

**E2011** — `parseAttribute()` does not validate argument count against the known attribute table. This is best left to the semantic pass (where known attributes and their arities are validated), but if you want the parser to catch it, add a check after parsing args against a static table of `{name → expected_arg_count}`. Currently completely unchecked.

---

### 5. Summary

| Category                | Count | Detail                                                                                  |
| ----------------------- | ----- | --------------------------------------------------------------------------------------- |
| **Critical**            | 1     | `consume()` never reports diagnostics — all 101 `consume()` calls are silent on failure |
| **Critical**            | 1     | `advance()`/`peek()` don't skip comments despite documentation claiming they do         |
| **Wrong code**          | 1     | `E3014` (semantic) used in parser for chained comparison; should be `E2026`             |
| **Missing code**        | 4     | `E2018`, `E2019`, `E2020`, `E2004` — right concept, wrong code (uses `E2001`)           |
| **Missing check**       | 4     | `E2016`, `E2021`, `E2022`, `E2029` — condition not checked at all, no error fired       |
| **Parser can't know**   | 2     | `E2011` (attr arg count), `E2017` (param groups after `->`) — deferred to semantic pass |
| **Semantic-phase only** | 2     | `E2024`, `E2025` — correct to leave for semantic pass                                   |