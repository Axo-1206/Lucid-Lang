# Parser Analysis Report

---

## 1. Bugs

### BUG-1: `PipelineStepKind` enum vs. parser usage mismatch (undefined behaviour)

**Files:** `ParserExpr.cpp`, `ExprAST.hpp`

`ExprAST.hpp` defines exactly 5 variants in `PipelineStepKind`:
```cpp
enum class PipelineStepKind {
    Ident, BehaviorRef, FieldRef, ArgPack, AnonFunc
};
```

But `parsePipelineStep()` sets kinds that **do not exist** in the enum:
```cpp
step->kind = PipelineStepKind::BehaviorArgPack;  // line ~1810 — NOT in enum
step->kind = PipelineStepKind::FieldArgPack;      // line ~1856 — NOT in enum
step->kind = PipelineStepKind::IndexArgPack;      // line ~1938 — NOT in enum
step->kind = PipelineStepKind::IndexRef;          // line ~1949 — NOT in enum
```

These are also stored in `step->index` which is a field not declared in `PipelineStepAST`:
```cpp
step->index = std::move(indexChain);  // 'index' field doesn't exist on PipelineStepAST
step->genericArgs = std::move(genericArgs); // 'genericArgs' field also absent
```

This is likely causing a **compilation failure** (linker or compile error) that is masked because these code paths may not be reached in current test inputs. Any pipeline step using field-as-argument-pack or array-index form will crash or silently corrupt the step node.

**Fix:** Either add the missing enum variants and struct fields to `PipelineStepAST`, or collapse them to existing variants and change the representation.

---

### BUG-2: `ARROW` token treated as a binary operator (silent wrong AST)

**File:** `ParserExpr.cpp` — `infixPrec()` and `parsePrattExpr()`

`infixPrec()` returns `PREC_PIPE` (3) for `ARROW` (`->`):
```cpp
case TokenType::ARROW: return PREC_PIPE;   // for '->'
```

In `parsePrattExpr()`, when the infix loop sees `|>` it calls `parsePipelineExpr`. But `ARROW` has no special case — it falls through to the **standard binary operator path**:
```cpp
advance(); // consume the operator
ExprPtr rhs = parsePrattExpr(nextPrec, ...);
// ...
auto node = arena_.make<BinaryExprAST>();
node->op = tokenToBinaryOp(opTok);  // ARROW has no case → returns BinaryOp::Add (the silent default)
```

So `x -> y` inside an expression (e.g., a return type annotation mistakenly placed in expression context) produces a `BinaryExprAST` with `op = BinaryOp::Add` — completely silently.

`ARROW` should either produce a proper error in `parsePrattExpr()` or be removed from `infixPrec()` entirely (returning `PREC_NONE`). The only legitimate infix use of `->` is in pipeline context, which is already handled by `parsePipelineExpr` via `PIPELINE` (`|>`), not `ARROW`.

---

### BUG-3: `parseReturnStmt` uses `ReturnStmtAST::values` but `ReturnStmtAST` has a single `value` field

**Files:** `ParserStmt.cpp`, `StmtAST.hpp`

`StmtAST.hpp` declares:
```cpp
struct ReturnStmtAST : StmtAST {
    std::vector<ExprPtr> values;   // ← vector, correct
    ...
};
```

`parseReturnStmt()` populates `node->values` — this is consistent. However, in `Annotator.cpp` the walker accesses:
```cpp
if (node.value) walk(node.value.get());  // line ~560 in Annotator.cpp
```
and in `SemanticStmt.cpp`:
```cpp
TypeAST* valType = checkExpr(node.value.get(), ...);  // single field access
```

These semantic files assume a single `value` field, while the parser puts multiple values into `values`. The `Annotator.cpp` and `SemanticStmt.cpp` references to `node.value` would be a compile error — unless the semantic layer has a different (older) `ReturnStmtAST` definition. This is an **API split** between the parser and the semantic phase; only one of them can be correct.

---

### BUG-4: `parseStmt` multi-var detection leaks `dummy` TypePtr through `pos_` restoration

**File:** `ParserStmt.cpp`, lines 42–60

```cpp
std::size_t savedPos = pos_;
advance(); // consume keyword
bool hasIdentifier = check(TokenType::IDENTIFIER);
if (hasIdentifier) advance();
bool hasType = looksLikeType();
TypePtr dummy = nullptr;
if (hasType) {
    dummy = parseType();   // <-- calls arena_.make<...>() which allocates in the arena
    if (!dummy) hasType = false;
}
bool nextIsComma = check(TokenType::COMMA);
pos_ = savedPos; // restore position
```

`pos_` is restored but the arena allocation that happened inside `parseType()` is **not rolled back**. The arena is a bump allocator with no rollback capability. The nodes allocated during this speculative parse are simply leaked into the arena and wasted. This isn't a crash, but in deeply nested or frequently-called contexts it inflates arena memory usage. A type that fails also triggers error messages during the speculative parse that could confuse the user (though in practice `looksLikeType()` is checked first so valid input is unlikely to trigger errors).

---

### BUG-5: `parsePipelineExpr` error message says `'->'` not `'|>'`

**File:** `ParserExpr.cpp`, line ~1625:
```cpp
errorAt(DiagCode::E2006, "pipeline '->' requires at least one step");
```

The pipeline operator in Luc is `|>`, not `->`. The error message is misleading. The code path that reaches this is already unreachable in practice (you can't get `parsePipelineExpr` called without consuming at least one `|>` step), but the message is still wrong.

---

### BUG-6: `parseParamGroup` — `UnknownTypeAST` check is unreachable for arena-allocated types

**File:** `Parser.cpp`, `parseParamGroup()`:
```cpp
if (paramType->isa<UnknownTypeAST>()) {
    errorAt(DiagCode::E2005, "invalid parameter type");
    break;
}
```

`parseBaseType()` never returns `nullptr` — it always returns an `UnknownTypeAST` node on failure (allocated in the arena). The `isa<UnknownTypeAST>()` check is correct in principle, but this means the `if (!dummy)` check on `dummy = parseType()` in `parseStmt` (BUG-4) will **never be true** — `parseType()` always returns a non-null pointer. The `hasType = false` branch is dead code.

---

## 2. Potential Infinite Loops

### LOOP-1: `parseArgList` can infinite-loop on persistent parse failure

**File:** `ParserExpr.cpp`, `parseArgList()`:
```cpp
while (!check(TokenType::RPAREN) && !isAtEnd()) {
    std::size_t savedPos = pos_;
    ExprPtr arg = parseExpr();

    if (pos_ == savedPos) {
        errorAt(...);
        if (!isAtEnd()) advance();   // one token consumed
        if (check(TokenType::COMMA)) advance();
        continue;    // <-- back to top of while
    }
    ...
}
```

If `advance()` consumes a token that is not `)` or EOF, and `parseExpr()` on the next iteration also makes no progress (e.g., a sequence of unsupported tokens), the loop will keep consuming one token at a time. This is technically not infinite, but for a long sequence of garbage tokens it will be extremely slow and produce a flood of error messages before recovering.

More critically: if `parseExpr()` returns an `UnknownExprAST` (which it does on error via `parsePrimaryExpr`) **and does advance** `pos_`, `savedPos != pos_` and the arg is pushed. The error path only triggers when `parseExpr` makes zero progress, which happens when `parsePrimaryExpr` emits an error and returns `UnknownExprAST` **without consuming a token** (see `parsePrimaryExpr`'s final fallthrough which does call `advance()` — so this is mostly safe, but not proven for all token sequences).

### LOOP-2: `parseBlock` — `synchronize()` may not advance past `RBRACE`

**File:** `ParserStmt.cpp`, `parseBlock()`:
```cpp
while (!check(TokenType::RBRACE) && !isAtEnd()) {
    if (match(TokenType::SEMICOLON)) continue;
    std::size_t savedPos = pos_;
    StmtPtr stmt = parseStmt();
    if (stmt) {
        ...
    } else {
        if (pos_ == savedPos) {
            advance();  // force progress
        }
        if (!check(TokenType::RBRACE) && !isAtEnd()) {
            synchronize();
        }
    }
}
```

`synchronize()` stops at `RBRACE` without consuming it. After `synchronize()` the outer `while` loop checks `!check(TokenType::RBRACE)` — if we are now sitting on `RBRACE`, the loop exits correctly. This is safe.

However, if `parseStmt()` returns a **non-null** but invalid statement (e.g., an `UnknownStmtAST`) **without advancing**, `pos_ == savedPos` is still false (the stmt was "parsed" by some internal error path that did advance). The inner `if (pos_ == savedPos) advance()` is skipped, and `synchronize()` is also skipped. On the next iteration, `parseStmt()` is called again with the same token, potentially looping. The safety net only fires when stmt is `nullptr`.

### LOOP-3: `parseStructDecl` field loop with broken synchronize

**File:** `ParserDecl.cpp`, `parseStructDecl()`:
```cpp
while (!check(TokenType::RBRACE) && !isAtEnd()) {
    match(TokenType::SEMICOLON);
    match(TokenType::COMMA);
    if (check(TokenType::RBRACE)) break;
    ...
    FieldDeclPtr field = parseFieldDecl();
    if (field) {
        node->fields.push_back(...);
    } else {
        synchronize();
    }
}
```

If `parseFieldDecl()` returns `nullptr` without consuming any token AND `synchronize()` also fails to advance (e.g., we're on an `AT_SIGN` or another boundary token that synchronize stops at immediately), the loop repeats with the exact same token. The `match(SEMICOLON)` and `match(COMMA)` at the top may not fire, and `parseFieldDecl()` will fail again, creating a tight infinite loop.

### LOOP-4: `parseSwitchCase` inner value loop stall

**File:** `ParserStmt.cpp`, `parseSwitchCase()`:
```cpp
while (check(TokenType::COMMA)) {
    advance(); // consume ','
    if (check(TokenType::COLON)) break;

    std::size_t savedPos = pos_;
    ExprPtr val = parsePrattExpr(0);
    if (pos_ == savedPos) {
        errorAt(...);
        advance(); // consume offending token
        break;
    }
    ...
}
```

This is safe with the `advance()` + `break` guard. However, if `parsePrattExpr(0)` returns `UnknownExprAST` **and** has advanced (reporting an error internally), the value is still pushed as a case value. Multiple consecutive error values can accumulate silently.

### LOOP-5: `parseGenericParams` stall detection is incomplete

**File:** `Parser.cpp`, `parseGenericParams()`:
```cpp
do {
    match(TokenType::COMMA);
    if (check(TokenType::GREATER)) break;
    std::size_t savedPos = pos_;
    GenericParamPtr gp = parseGenericParam();
    if (!gp) {
        if (pos_ == savedPos) {
            errorAt(...);
            advance();
            stalled = true;
            break;
        }
        continue;
    }
    params.push_back(...);
} while (!check(TokenType::GREATER) && !isAtEnd());
```

If `parseGenericParam()` returns `nullptr` but **did** advance (unlikely but possible via error recovery inside), the `continue` causes the loop to retry. If the next token is not `>` and not EOF, and `parseGenericParam` keeps returning `nullptr` with progress, this loops until EOF. The stall guard only fires when `pos_ == savedPos`.

---

## 3. Complexity Reduction Opportunities

### REFACTOR-1: `parsePipelineStep` — split into sub-functions (600+ lines, cyclomatic complexity ~20+)

`parsePipelineStep` is by far the most complex function in the codebase. It handles 8+ distinct cases with interleaved lookahead, parsing, and error recovery. Suggested decomposition:

```cpp
// Current: one monolithic function
PipelineStepPtr parsePipelineStep();

// Proposed: dispatch + specialised parsers
PipelineStepPtr parsePipelineStep() {
    if (isAnonFuncAhead())       return parsePipelineAnonFunc();
    if (!check(IDENTIFIER) && !isPrimitiveTypeToken(peek().type))
                                 return parsePipelineError();
    std::string name = consumeName();
    auto genericArgs = maybeParseGenericArgs();
    if (check(COLON))            return parsePipelineBehaviorRef(name, genericArgs);
    if (check(DOT))              return parsePipelineFieldRef(name, genericArgs);
    if (check(LBRACKET))         return parsePipelineIndexRef(name, genericArgs);
    if (check(LPAREN))           return parsePipelineArgPack(name, genericArgs);
    return parsePipelineIdent(name, genericArgs);
}
```

The lookahead for anon-func detection (the `testPos` scanning block) should be extracted as:
```cpp
bool Parser::isAnonFuncAhead() const;
```

This replaces the 60-line inline lookahead scan with a named, testable predicate.

---

### REFACTOR-2: `parsePrattExpr` special-case branches — extract named handlers

`parsePrattExpr`'s main loop has 7 `if` branches for special infix operators before reaching the generic binary-op path. Each branch is 5–15 lines. Extract them:

```cpp
// Instead of inline if-chains, dispatch via named methods:
if (isAssignOp(opTok))               { lhs = parseInfixAssign(lhs); break; }
if (opTok == TokenType::IS)          { lhs = parseInfixIs(lhs); continue; }
if (opTok == TokenType::PIPELINE)    { lhs = parsePipelineExpr(lhs); continue; }
if (opTok == TokenType::COMPOSE)     { lhs = parseComposeExpr(lhs); continue; }
if (opTok == TokenType::QUESTION_QUESTION) { lhs = parseNullCoalesce(lhs); break; }
lhs = parseInfixBinary(lhs, opTok);  // generic path
```

---

### REFACTOR-3: `parseReturnList` — the function-type vs multi-return disambiguation is brittle

`parseReturnList` uses `peekNext().type` and `peekAt(2).type` to distinguish `-> (x int) -> int` (single function type) from `-> (int, string)` (multi-return). The heuristic is:
- If `(` followed by `IDENTIFIER` followed by a type-start → function type
- If `(` followed by `)` followed by `->` → function type

This misidentifies `-> (SomeType, Other)` if `SomeType` happens to be followed by something that looks like a type (which a named type would be). An explicit grammar disambiguation token (e.g., always requiring `->` before multi-return types, or using a distinct delimiter) would be cleaner. At minimum, add a comment documenting the known false-positive cases.

---

### REFACTOR-4: `parseStmt` multi-var speculative parse — use pure lookahead instead of arena allocation

As noted in BUG-4, `parseStmt` calls `parseType()` speculatively then restores `pos_`. Replace with a pure-lookahead check that doesn't touch the arena:

```cpp
// Replace the speculative parse with a token-scan predicate:
static bool looksLikeMultiVarDecl(const std::vector<Token>& tokens, std::size_t pos) {
    // consume keyword (1 token)
    // check IDENTIFIER (1 token)
    // check type start (1+ tokens via isPrimitiveTypeToken or IDENTIFIER)
    // check COMMA
    // ... all without parsing, just token inspection
}
```

This is the same approach already used by `looksLikeMultiAssignStart()` and `looksLikeFuncDecl()`.

---

### REFACTOR-5: `parseAttributes` / `parseAttribute` — argument parsing duplicates literal-token logic

`parseAttribute`'s inner loop for parsing attribute arguments duplicates the literal-token handling that exists in `parseLiteralExpr`. The 5 token-type checks (`STRING_LITERAL`, `INT_LITERAL`, `HEX_LITERAL`, etc.) should call a shared `parseAttributeArgLiteral()` helper to avoid the duplication and keep the two in sync (e.g., if a new literal type is added to the lexer).

---

### REFACTOR-6: Lookahead helpers share duplicated comment-skipping loops

All lookahead functions (`looksLikeFuncDecl`, `looksLikeAnonFunc`, `looksLikeStructLiteral`, `isAnonFuncAhead` in `parsePipelineStep`) contain the same comment-skipping loop:
```cpp
while (i < tokens_.size() && (tokens_[i].type == TokenType::LINE_COMMENT ||
                               tokens_[i].type == TokenType::DOC_COMMENT))
    ++i;
```

This should be a shared inline helper:
```cpp
void skipComments(std::size_t& i) const {
    while (i < tokens_.size() &&
           (tokens_[i].type == TokenType::LINE_COMMENT ||
            tokens_[i].type == TokenType::DOC_COMMENT))
        ++i;
}
```

This appears at least 15 times across the lookahead functions.

---

## Summary Table

| ID         | Category       | File                             | Severity                                                               |
| ---------- | -------------- | -------------------------------- | ---------------------------------------------------------------------- |
| BUG-1      | Bug            | `ParserExpr.cpp` / `ExprAST.hpp` | **Critical** — undefined enum values, missing struct fields            |
| BUG-2      | Bug            | `ParserExpr.cpp`                 | **High** — `ARROW` silently produces wrong `BinaryExprAST`             |
| BUG-3      | Bug            | `ParserStmt.cpp` / `StmtAST.hpp` | **High** — `values` vs `value` field split between parser and semantic |
| BUG-4      | Bug            | `ParserStmt.cpp`                 | **Medium** — speculative arena allocation not rolled back              |
| BUG-5      | Bug            | `ParserExpr.cpp`                 | **Low** — wrong operator name in error message                         |
| BUG-6      | Bug            | `Parser.cpp`                     | **Low** — dead `if (!dummy)` branch                                    |
| LOOP-1     | Potential loop | `ParserExpr.cpp`                 | **Medium** — slow degradation on garbage input                         |
| LOOP-2     | Potential loop | `ParserStmt.cpp`                 | **Medium** — non-null bad stmts bypass progress guard                  |
| LOOP-3     | Potential loop | `ParserDecl.cpp`                 | **Medium** — struct field loop with weak recovery                      |
| LOOP-4     | Potential loop | `ParserStmt.cpp`                 | **Low** — silent error accumulation in switch cases                    |
| LOOP-5     | Potential loop | `Parser.cpp`                     | **Low** — generic param loop on advancing failures                     |
| REFACTOR-1 | Complexity     | `ParserExpr.cpp`                 | Split `parsePipelineStep` (~8 responsibilities)                        |
| REFACTOR-2 | Complexity     | `ParserExpr.cpp`                 | Extract `parsePrattExpr` infix handlers                                |
| REFACTOR-3 | Complexity     | `Parser.cpp`                     | Replace brittle `parseReturnList` heuristic                            |
| REFACTOR-4 | Complexity     | `ParserStmt.cpp`                 | Replace speculative `parseType` with pure lookahead                    |
| REFACTOR-5 | Complexity     | `ParserDecl.cpp`                 | Deduplicate attribute argument literal parsing                         |
| REFACTOR-6 | Complexity     | All lookahead                    | Extract comment-skipping into shared helper                            |
| REFACTOR-7 | Complexity     | `DeclAST.hpp` + semantic         | Unify `sig` / `type` naming on `FuncDeclAST`                           |